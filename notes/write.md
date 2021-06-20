---
title: 'LevelDB: Write Path'
date: 2021-06-18 22:11:47
tags:
cover_img:
feature_img:
description:
keywords:
category:
---

# LevelDB WriteBatch

这篇文章意料之内的写得很乱。感觉不太适合刚看代码的来读，看代码有困惑的地方可以来这里互相印证一下。

## 最简单的写路径: 写 Memtable

https://zhuanlan.zhihu.com/p/143309915

可以参考上面这篇 blog. LevelDB 会尝试写入 memtable. 这里比较重要的链路是：

1. `DBImpl::Put` / `DBImpl::Delete` 实现了 `DB` 对外的接口, 它们会把请求整理到一个 `leveldb::WriteBatch` 对象中. `batch` 可以 `Put`, `Delete`, 操作会被放到 `batch` 中的一个 `std::string` 中.  WriteBatch 开头是一个 `kHeader` 长度的 header, 包含 8-byte 的 seq-number 和 4-byte 的 count. 随后是记录列表, 每条记录包含一个 `char` 和 `length` + `content` 的内容。
2. `DBImpl::Write`  的时候, 会确定一下是不是 `sync` 写。一个 batch 内全是 sync, 这个 batch 会选出一个 leader 来写入数据。
   1. 写入数据是单线程写的，一般链路上会拿到 `DBImpl::mutex_` . 
   2. 调用 `MakeRoomForWrite`, 确保写入空间是充足的。
      1. 如果 L0 文件超过 8 个文件，到达了 soft limit, 它会释放 mutex_ 并 sleep 1ms. 等待 Compaction 成功
      2. 在可能的 sleep 后，如果 `mem_` 内存充足 \(判定的方式是 Arena 的内存占用小于某个阈值\)，它会直接返回，开启后续流程。
      3. `imm_` 不是 null 的，基本上就说明后台有 compaction，需要等待 `imm_` 被压缩
      4. 看看是否是 L0 文件到达上限，到达了就停止写
      5. 走到这里说明，imm_ is nullptr, 文件可能大于 soft_limit, 一定小于 hard limit，同时, mem_ 内存是不够的，需要切新的文件，然后把旧的文件放到 `imm_` 中。这里操作比较细，需要：
         1. `Versions::NextNewFileId` 拿到系统的下一个文件 id, 作为 log 文件的 id, 让系统创建一个新的文件作为新的 log 文件.
         2. `imm_` 指向 `mem_`, 这里不会更新 `ref_count`.
         3. 创建一个新 `mem_`, 并把日志指向新创建的文件，对他 `Ref()` 一下，增加 ref count
         4. 调度 `MaybeScheduleCompaction` , 正常情况会在 2.2.2 里面成功退出.
   3. 拿到 `Versions::LastSequence` ，作为这次的 log_id. 因为写 log 的线程只有一个，所以这个是写入安全的。这里会放锁然后尝试顺序写 log.
   4. 写入成功后，插入 mem. 这里没有带 `mutex_`, 但是只有单个线程去插入。`!w.done` 的时候，写者会 `Wait`，所以不会有别的线程来写入，但是这里可以插入到写入的队列中。写完之后放锁。
   5. 写完之后 notify waiting 的记录。

写入就这样轻松快乐的完成了。

### 内存操作：和读取的关系

我们之前说了 `LastSequence` ，读取的时候，如果没有快照，会 grab lock，然后拿到 `VersionSet::LastSequence`. 这个时候，只有写日志会推高这个 Sequence, 用这个来读取是安全的，不会读到新写入的数据。

同时，这个地方读会给 `mem_` 和 `imm_` 来做 `Ref`, 这个时候，内存是最后一个读者带着 `mutex_` 来释放的。

## Memtable Compaction

`DBImpl::MaybeScheduleCompaction` 是可能调度 Compaction 的函数，在必要的时候，处理 Compaction.

调用它的地方有：

1. 手动 `CompactRange` 的时候，在 `TEST_CompactRange`
2. `Get` / ReadSample 的时候，看看有没有触发 Seek Compaction
3. 写入产生 `imm_` 的时候
4. 每次 Compaction 完后，再次尝试看看有没有 Compaction

Compaction 的条件有：

1. `imm_` 不为 nullptr
2. 有 ManualCompaction
3. `VersionSet::NeedsCompaction`

Compaction 有下面几种类型和优先级:
1. CompactMemTable: 优先级最高
2. ManualCompaction: 优先级次高
3. Size Compaction: 优先级比 SeekCompaction 高
4. SeekCompaction

我们简单介绍一下 Compaction 的大概的逻辑：

### Memtable Compaction

1. `DBImpl::MaybeScheduleCompaction`: `env_->Schedule(&DBImpl::BGWork, this);` , 触发 Compaction
2. `BackgroundCall()` , 持有 `mutex_`

以 `CompactMemTable` 为例，讲一下基本的 Compaction 流程：

1. 拿到 `VersionSet::current`, 作为 Compaction 的 base. 在 Compaction 期间，不会有别的 Compaction 进来。然后 `Ref()` 它一下（我感觉其实没啥意思...）
2. 进入 `DBImpl::WriteLevel0Table` 创建一个 `FileMeta` 然后 `VersionSet::NewFileNumber()`, 拿到新 SST 的文件 id, 把它放到 `pending_outputs_` 里面。
   1. 释放锁
   2. 创建 `TableBuilder`，然后从 iterator 中读取数据
   3. 如果成功，Sync 并关闭文件，否则，删除 SST 然后返回
   4. 把新文件丢到 table cache 中。
   5. 重新持有锁
3. `pending_outputs_` 移除新 SST
4. 调用 `Version::PickLevelForMemTableOutput` 获得文件写入的层数。这里注意，memtable compaction 不一定会直接下到 L0 层，可能直接第二层了。
5. VersionEdit 记录新创建文件的信息，包括 key 范围和在哪层。
6. Unref base (我一下没看出有啥用)
7. edit 再记录一下 log file number, 这里注意，只有 memory compaction 会推高它，原因后面介绍。
8. 准备写元数据，注意`VersionSet::Finalize` 的时候，这里会更新 compaction score, 用来处理可能的 size compaction.
9. LogAndApply 会更新一些别的信息，然后写 MANIFEST, 更新 Version 和元信息。这里还会处理 Version 链表里 Ref 之类的东西
10. Compaction 完后，Unref `imm_` (还记得不，前面写内存的时候，我们提到了，Compaction 的时候，`mem_` 会被 `Ref` 一下，`mem_` 变 `imm_` 的时候没有 `Unref`, 这个时候 `Unref` 啦)。
11. 调用 `RemoveObsoleteFiles`, 这个函数 在 db 初始化和文件打开的时候调用，清楚掉旧的文件。
    1. 日志文件: prev_log_number 是一个废弃的字段. log_number 会被内存 compaction 推高，大于它才会被回收.
    2.  SST(.sst, .ldb) 文件: 不是 pending_outputs_ 且不存在于 VersionSet 所有版本引用的文件中的文件。. (还记得我们的 `pending_outputs_` 吗，刚刚几行提到的)
12. 都写完就结束啦

### Compaction 

Memtable Compaction 应该是最简单的 Compaction 了。下面讲讲剩下的。这些 Compaction 往往涉及从某层的某个 memtable 开始，然后扩大范围，最后 Compaction.

再回到 `DBImpl::BackgroundCompaction`:

1. `VersionSet::PickCompaction` 来挑选 Compaction，它会选出对应的 Seek Compaction 和 Range Compaction. 还记得我们在上面 `8` 提到的 Compaction Score 吗？Size Compaction 就是靠这个 Score 来选出 Compaction 的 **level** 和 **起点 SST** 的。
   1. Compaction 会计算出一个 Compaction Score, score 最大的是下一个 size compaction 的目标。
   2. 这里还有个 `compaction_pointer_` 这个是轮转的。上次这一层 Compaction 到哪，下次就从这个 `pointer` 开始了
2. 如果是 Level0 到下层的 Compaction，这里需要把 L0 中，key 重复的一起 Compaction 掉。
   1. 这里调用了 `Version::GetOverlappingInputs`, 这个函数做的很粗糙，对 L0 就不停扩大 L0 范围。感觉很不优雅
3. `VersionSet::SetupOtherInputs` 处理本层和下一层的其余输入 SST：
   1. `AddBoundaryInputs` 再次添加**来源层**的 inputs。这是说，对于 SST (实际上 L0 层应该不会出现这个问题），写入比 **起点 SST** 早的文件捞出来一起 Compaction 掉，否则就会出现同一个 key，上层比下层新的 case.
   2. 根据目标层的所有 SST，拿到它最小的、最大的 `InternalKey`. 然后再调用 `Version::GetOverlappingInputs` 去 **下一层** (可能不是目标层哦)，找到范围对应的 SST。然后再和 3.1 一样走一次 `AddBoundaryInputs`.
   3. 现在，**来源层** 和 **来源层 + 1** 的 SST 文件数量没有超过 `ExpandedCompactionByteSizeLimit(options_)` 的话，还可以从来源层再捞一把和下层的边界有交集的数据...
   4. 把 **来源层 + 2** 范围重合的文件也计算一遍。
   5. 给 **来源层** 和 **来源层 + 1** 设置 Compaction Pointer （记得 1.2 不）
4. 如果来源层 + 1 没有重叠，来源层只有一个文件，可以 `Compaction::IsTrivialMove` 看出来, 然后直接推到下层。
5. \(4\) 不成功的情况下，调用 `DBImpl::DoCompactionWork` 来 Compaction. 他可能输出多个 SST，所以搞了个 `DBImpl::CompactionState` 来维护状态。
   1. 找到 `DBImpl::snapshots_`, 拿到最老读者的 seq id. 这个 seq id 之前的记录是不能删除的。
   2. 如果有 `imm_` ，放锁，然后优先 Compact Memtable. 这是为了不影响正常写入
   3. 如果这次写的 SST 太大了，先输出，然后换个 SST 写
   4. 下面是正式写数据的流程，这里要决定数据是不是要保留。大致逻辑是，需要不影响 Compaction 的正确性。总的来说，这个删除的策略感觉是非常保守的。

```c++
    // Handle key/value, add to state, etc.
    //
    // 每次保留一个 current_user_key. 遇到第二次
    bool drop = false;
    if (!ParseInternalKey(key, &ikey)) {
      // Do not hide error keys
      current_user_key.clear();
      has_current_user_key = false;
      last_sequence_for_key = kMaxSequenceNumber;
    } else {
      if (!has_current_user_key ||
          user_comparator()->Compare(ikey.user_key, Slice(current_user_key)) !=
              0) {
        // First occurrence of this user key
        current_user_key.assign(ikey.user_key.data(), ikey.user_key.size());
        has_current_user_key = true;
        last_sequence_for_key = kMaxSequenceNumber;
      }

      // key 保留情况处理.
      if (last_sequence_for_key <= compact->smallest_snapshot) {
        // Hidden by an newer entry for same user key
        drop = true;  // (A)
      } else if (ikey.type == kTypeDeletion &&
                 ikey.sequence <= compact->smallest_snapshot &&
                 compact->compaction->IsBaseLevelForKey(ikey.user_key)) {
        // Note(mwish): `IsBaseLevelForKey` 这个函数也太讨巧了，感觉不太好用...
        // 终于知道为什么要 Seek Compaction 了.
        //
        // For this user key:
        // (1) there is no data in higher levels
        // (2) data in lower levels will have larger sequence numbers
        // (3) data in layers that are being compacted here and have
        //     smaller sequence numbers will be dropped in the next
        //     few iterations of this loop (by rule (A) above).
        // Therefore this deletion marker is obsolete and can be dropped.
        drop = true;
      }

      last_sequence_for_key = ikey.sequence;
    }
```



### Compaction 和读

读取的逻辑在：https://zhuanlan.zhihu.com/p/372152739 这里介绍过一些。

Memtable 和 LogSequence 已经介绍过了，这里介绍 SST 相关的。Compaction 不会直接删除文件，但是会 `RemoveObsoleteFiles`。对于读而言，如果哪个 snapshot 和 version 正在被读取，是不会被删掉的。

那我们考虑一个问题。有一个巨早拿到的 Snapshot, 过了很久之后被读取。这个时候拿到的 `Version` 是 `current`. 这个地方，怎么保证被删掉的文件数据还能读呢？这个是靠 Compaction 的时候，读到的 log_id 保证的。假如我们有一个很早的 snapshot, 它的 SST 会被删除，但是相关的记录是不会被删除的。

---

* LevelDB Log: WAL of LSMTree C0  https://zhuanlan.zhihu.com/p/145178907
* LevelDB Put: How it Batch https://zhuanlan.zhihu.com/p/143309915
* LevelDB Memtable https://zhuanlan.zhihu.com/p/145403978


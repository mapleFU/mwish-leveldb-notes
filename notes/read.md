---
title: 'LevelDB: Read path'
date: 2021-05-13 23:22:44
tags:
cover_img:
feature_img:
description:
keywords:
category:
---

鉴于网上讲 LevelDB 代码的文章没有一万也有一千了，本文肯定比他们还烂。所以这篇文章就简单讲讲 LevelDB 的读流程。

我们对这个的事先印象是：

1. 用户 `Get` 一个 key
2. 从 memtable 里面找这个 `key` . memtable 里面，key 在写入的时候，被 `WriteBatch::Put` 变成了 `<写类型, key 长度, key 内容, value 长度, value 内容>` 的字符串，然后在 `MemTableInserter::Put` 和 `Memtable::Add` 里面变成了 `<sequence, type, key 长度, key 内容>` `<value 长度, value 内容> ` 的 `InternalKey` 和 `value`
3. 如果在正在写入的 memtable 和 immutable memtable 都没有，那么它回到 SST 里面去查找。

此外，读可以拿到 iterator 和 snapshot, 并且用 iterator 和 snapshot 来读：

leveldb 可以拿到 iterator 读，也可以拿到 snapshot 去读，注意这里还是有生命周期：

```c++
leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
for (it->SeekToFirst(); it->Valid(); it->Next()) {
  cout << it->key().ToString() << ": "  << it->value().ToString() << endl;
}
assert(it->status().ok());  // Check for any errors found during the scan
delete it;
```

和 snapshot

```c++
leveldb::ReadOptions options;
options.snapshot = db->GetSnapshot();
... apply some updates to db ...
leveldb::Iterator* iter = db->NewIterator(options);
... read using iter to view the state when the snapshot was created ...
delete iter;
db->ReleaseSnapshot(options.snapshot);
```

LevelDB 内部大量使用 `slice` 类型，表示“没有所有权的字符串”。这个有点类似 `std::string_view`, 不过功能弱不少。

那么，我们今天要给上面的流程填充一些细节：

1. 读文件相关的 system api
2. LevelDB 对 SST 相关的读处理：
   1. SST 文件的读取
   2. Block Cache / mmap
   3. Iterator 类型
3. LevelDB 读相关的内容回收

### System API && env

env 定义在文件的 `util/env.h` 和 `util/env_{平台}.cc` 下面, 定义了这个平台默认的行为。和文件相关的内容全部封装在这个接口里。

这里定义了：

 `RandomAccessFile` 随机读的文件，用 `pread` 或者 `mmap` 来实现。提供一个带 `offset` 和 `size` 的 read 接口；需要注意的是，这里使用了 Limit, 限制数量为 1000 的 `mmap` 的 `RandomAccessFile` （这个配置参数是可以修改的）。作者认为：

1. mmap 本身可以在 random access 的情况下优化读（感觉这是因为 LevelDB 本身 Block Cache 使用的 LRU 策略比较简单）
2. Mmap 本身限制在了 1000 个：作者认为这样可以优化性能。我看了一下相关的讨论，作者认为 leveldb 本身存储的是小文件，而 compaction 过多导致系统 `RSS` 会变得巨大。所以作者把这个量限制在了 1000 个

```c++
// A file abstraction for randomly reading the contents of a file.
class LEVELDB_EXPORT RandomAccessFile {
 public:
  RandomAccessFile() = default;

  RandomAccessFile(const RandomAccessFile&) = delete;
  RandomAccessFile& operator=(const RandomAccessFile&) = delete;

  virtual ~RandomAccessFile();

  // Read up to "n" bytes from the file starting at "offset".
  // "scratch[0..n-1]" may be written by this routine.  Sets "*result"
  // to the data that was read (including if fewer than "n" bytes were
  // successfully read).  May set "*result" to point at data in
  // "scratch[0..n-1]", so "scratch[0..n-1]" must be live when
  // "*result" is used.  If an error was encountered, returns a non-OK
  // status.
  //
  // Safe for concurrent use by multiple threads.
  virtual Status Read(uint64_t offset, size_t n, Slice* result,
                      char* scratch) const = 0;
};
```

LevelDB 调用 `NewRandomAccessFile` 的时候，会先尝试走 `PosixMmapReadableFile` ，没有就 fallback 到 `PosixRandomAccessFile`.

LevelDB 给整个需要的链路都加入了 `Env*`, 来支持各个平台系统方面的调用

### Block Read

LevelDB 的数据是以 SST 组织的，SST 内切分成了不同的 block, 以便提供 Index/CRC/缓存。Block Size 和性能有比较大的关系。LevelDB 文档里面提示：

1. Scan 居多的话，那么 Block Size 可以设置大一点。
2. Point Get 居多的话，Block Size 可以设小一点。

RocksDB 文档也有对应内容。

LevelDB 用户可以配置 BlockCache, 而且一定有\(不用配置\) TableCache. 关于 LevelDB 的 Cache, 可以简单看看：

https://zhuanlan.zhihu.com/p/363382898

前者是 Block 内容的缓存，后者是 SST 的元信息的缓存。简单的说，可以通过后者索引到前者。

Block cache: `options.block_cache` 设置读文件的时候用的 block cache, 当执行一些不希望用到 block cache 的操作的时候，可以使用 `options.fill_cache = false` 的配置。

那么回到程序中，具体读取文件的时候，程序会从 `TableCache::Get` 来从元信息定位到 Block 相关的内容。然后调用 `Table::BlockReader` 和 `ReadBlock`. 在 ReadBlock 的时候，程序对上述的 `mmap` 和 用`pread`的文件作出了区分：

1. 如果没有提供 `block_cache`, 那么不会存 block cache, 但是 `mmap` 仍然可能使用
2. 如果提供了 `block_cache`:
   1. 如果是 `mmap` 的文件，不会走 Block cache
   2. 否则把内容缓存到 block cache 里，使用的 charge 是 compression 之前的 size.

### Iterators

`Iterator` 接口定义在 `include/leveldb/iterator.h`, 是 LevelDB 里面一个很包罗万象的类型。跟 Iterator 相关的类型有：

1. `DBIter`, 这个接口提供的 `key()` `value()` 都是 user 的 key, value, 提供给上层用户使用。
2. `MemTableIterator`: 给 `MemTable::Table::Iterator` 封装了一层，后者的 `key()` 包含了 `InternalKey` 和 `value` , 这个 `MemTableIterator` 对外提供 `InternalKey` 和 `value`
3. `Block::Iter`: 这个类型是给单个 `data block`  和 `index block`准备的（leveldb 还有一些存放别的元信息的 block）: 会走上一节我们提到的 `ReadBlock` 相关的接口，从 Block 里面读取 `InternalKey` 和 `value`
   1. 这里 Data Block 存放数据，index block 存放对 data block 的索引。

![C32EA624-79EA-43BD-8E24-1D018E3212A3](static/2021-05-14/C32EA624-79EA-43BD-8E24-1D018E3212A3.png)

如果说上面三个 `Iterator` 还比较清晰，那我们可以看看下面的一个奇怪的 Iterator:

1. `Version::LevelFileNumIterator`: 这个迭代器很奇怪（你马上就知道为什么了），它丢出的信息是 Iterator 的元信息(不是用户的 `key`, `value`, 而是 "文件的 <最大key>" 和 `<(文件号, 文件大小)>`

下面，是时候组织起这些 Iterator 了：

`TwoLevelIterator` : 重点来了，难得来了！`TwoLevelIterator` 提供了一种“两级” 的抽象：一个提供索引，一个提供数据。提供数据的结束之后，提供索引的去找下一项，具体使用的地方有两个：

1. `Table::NewIterator`: Table 创建的是 TwoLevelIterator, 首先是 IndexBlock 的 iterator, 然后是对应的 block reader. 实际上这里就是拿到 index 信息，然后能够读到 block 的信息。
2. 读某一层的数据。index_iter 是单层文件元信息的迭代器 `Version::LevelFileNumIterator`，BlockFunction 是 (1) 中的整合。见 `Version::NewConcatenatingIterator`.

怎么样，是不是连起来了～

再来一个：

```c++
Iterator* NewMergingIterator(const Comparator* comparator, Iterator** children,
                             int n);
```

这个地方会创建一个 `MergingIterator`, 合并所有的 Iterator, 找到 `key` 最小的一个。需要注意的是，目前使用的时候，这里 `key` 最小指的还是 `InternalKey`.

此外，还要注意 `RegisterCleanup`, 这个函数是 `Iterator` 清理自身资源用的。

### Version && snapshot

我们上面提供了各种各样的 `Iterator`, 这下我们读的时候可有工具了。但是这里需要注意的是，读的时候还可以选择一个 snapshot, 然后，还需要考虑读的时候，中途发生了 compaction 要怎么处理。

LevelDB 提供了一个 `sequence_id` 来指定顺序。它定义的是读相对于写的偏序, 他存储在 `VersionSet` 里：

1. 写会推高 `last_sequence_`, 而且是一次性推高很多，等同于写入的 batch
2. 读会拿到 `last_sequence_` , 并根据这个值来决定自己读到什么。

实际上，即使不外部创建 Snapshot, 内部也会拿到一个最近的 sequence id, 来决定这个时候读的 Version, 即文件的一致性快照。

LevelDB 拿到了这个 `sequence_id` 之后, 需要接下来决定自己要读什么。然后它会找到最新的 `Version`. `Version` 是 LevelDB 中 SST 文件布局的一个版本，有任何的 compaction 发生，都会生成新的 Version。那么为什么它每次都会找到新的 Version, 而不是根据 snapshot 来找版本链呢？实际上，因为我们说的，snapshot 会添加一个 Snapshot, 读的时候只会根据 latest snapshot 拿到一个版本, 然后根据这个版本去做一致性读.

拿到 Version 之后，单个 Version 里面，内容是不变的。Version 可以提供每层的 `Iterator`, 拿到这些就可以构建 `MergingIterator` 去读啦~

### 资源回收

实际上，LevelDB 大部分采用了读者来回收内存的模式，同时使用了很多引用计数。这些引用计数通常都不是线程安全的。

很多类提供了 `Ref()` `Unref()` 的方法。感觉 LevelDB 最好提供个侵入式智能指针（虽然它没有）。

那么在读的时候，`Version`, `Memtable` 对象会被 `Ref()` 一下。然后，有的时候是手动`Unref`, 有的时候，是给 `Iterator` 使用了 `RegisterCleanup`，让它们在正确的时间 `Unref`. 

### 对 Compaction 的影响

读行为会启发式的导致 seek compaction. 这个可以看到 `DBIter` 和  `Stats`相关的代码，有两种更新方式：

1. 点查的时候，如果这层 SST 没找到对应的内容（同时 bloom filter 也没有回避这次读），而下层有，则记录
2. seek 的时候，会有一个采样算法，记录

当记录到达一定值的时候，leveldb 认为应该错峰的去 compaction，这个叫 Seek Compaction.



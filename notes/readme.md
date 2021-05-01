# LevelDB Read/Write and compaction



大致路径：

1. LevelDB 使用
2. read path
   1. Scan path
   2. point get path
3. write path
   1. compaction
   2. reading
4. 杂项
   1. open/close

一些要懂的结构：SSTable, Table, BlockCache

一些要懂的内容：Compaction

### Common operations

见 [index.md](https://github.com/mapleFU/mwish-leveldb-notes/blob/master/doc/index.md):

```c++
Status DB::Open(const Options& options, const std::string& dbname, DB** dbptr)
```

调用 `DB::Open` 打开一个数据库。`Options` 可以设置打开时的参数、运行时的 block cache 等参数。

LevelDB 的操作状态被包装在一个 `leveldb::Status` 里面（这个做法很 Google）。Slice 的逻辑比较简单，可以简单介绍一下：它分为 `ok` 状态和一些不合法的状态，比如 `corruption`, `IOError`. 它只可能是这些状态中的一个。此外它可能带有一个描述自身状态的 `const char*`, 这个生命周期是由它管理的。

关闭数据库只需要 `delete db;` 就行了（其实我觉得创建的时候传进去一个 `DB** dbptr` 还是蛮诡异的）

LevelDB 的读写有基本的 `Read` `Put` `Update` 接口，而读写提供了不同的 `Options`:

```c++
std::string value;
leveldb::Status s = db->Get(leveldb::ReadOptions(), key1, &value);
if (s.ok()) s = db->Put(leveldb::WriteOptions(), key2, value);
if (s.ok()) s = db->Delete(leveldb::WriteOptions(), key1);
```

`ReadOptions` 和 `WriteOptions` 说实话都比较简单：

```c++
// Options that control read operations
struct LEVELDB_EXPORT ReadOptions {
  ReadOptions() = default;

  // If true, all data read from underlying storage will be
  // verified against corresponding checksums.
  bool verify_checksums = false;

  // Should the data read for this iteration be cached in memory?
  // Callers may wish to set this field to false for bulk scans.
  bool fill_cache = true;

  // If "snapshot" is non-null, read as of the supplied snapshot
  // (which must belong to the DB that is being read and which must
  // not have been released).  If "snapshot" is null, use an implicit
  // snapshot of the state at the beginning of this read operation.
  const Snapshot* snapshot = nullptr;
};

// Options that control write operations
struct LEVELDB_EXPORT WriteOptions {
  WriteOptions() = default;

  // If true, the write will be flushed from the operating system
  // buffer cache (by calling WritableFile::Sync()) before the write
  // is considered complete.  If this flag is true, writes will be
  // slower.
  //
  // If this flag is false, and the machine crashes, some recent
  // writes may be lost.  Note that if it is just the process that
  // crashes (i.e., the machine does not reboot), no writes will be
  // lost even if sync==false.
  //
  // In other words, a DB write with sync==false has similar
  // crash semantics as the "write()" system call.  A DB write
  // with sync==true has similar crash semantics to a "write()"
  // system call followed by "fsync()".
  bool sync = false;
};
```

这几个意义也很明确（当然，以后看代码还会碰到）

在一个 WriteBatch 里面的写，可以被视作原子写（但是 leveldb 不提供事务语义，只有 RocksDB 提供）：

\(而且众所周知，leveldb 一个正常的请求也会走一次选举，生成一个 WriteBatch )

```c++
leveldb::Status s = db->Get(leveldb::ReadOptions(), key1, &value);
if (s.ok()) {
  leveldb::WriteBatch batch;
  batch.Delete(key1);
  batch.Put(key2, value);
  s = db->Write(leveldb::WriteOptions(), &batch);
}
```

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

此外，用户可以自己定制 comparator, 需要继承 `leveldb::Comparator` 这一接口类型。这个 comparator 也可以作为 `Options` 丢到 db 内容里. 然后这里提到了 comparator 的兼容性：

```
The result of the comparator's Name method is attached to the database when it is created, and is checked on every subsequent database open. If the name changes, the leveldb::DB::Open call will fail. Therefore, change the name if and only if the new key format and comparison function are incompatible with existing databases, and it is ok to discard the contents of all existing databases.

You can however still gradually evolve your key format over time with a little bit of pre-planning. For example, you could store a version number at the end of each key (one byte should suffice for most uses). When you wish to switch to a new key format (e.g., adding an optional third part to the keys processed by TwoPartComparator), (a) keep the same comparator name (b) increment the version number for new keys (c) change the comparator function so it uses the version numbers found in the keys to decide how to interpret them.
```

#### config about performance

leveldb 除了有 open 时候对应行为的配置，也有性能相关的配置

1. block size: block 是 leveldb 缓存组织的形式。默认的 block size 为 compaction 之前的 4096 bytes. 系统建议，如果 point get 占主要 workload, 可以把这个设小一点，否则可以设大一点。（TODO: 这个会影响写入的时候的 block 吗？）
2. Compression: 通俗的说，compaction 是读写的时候的 CPU 换压缩空间。`snappy` 是 Google 发明的压缩算法。这里可以把这个算法换成 `no compression`, 来加快写，减小 CPU.
3. Block cache: `options.block_cache` 设置读文件的时候用的 block cache, 当执行一些不希望用到 block cache 的操作的时候，可以使用 `options.fill_cache = false` 的配置。
4. 使用 `filter_policy`, 这个会在写入的时候，在 compaction 阶段略微会占用一点开销，但是能大大节省 point get 的开销。

>  Suppose all Bloom filters have *M* bits in total and have the same false positive rate across all levels, with N total keys, each bloom filter will has a false positive rate $O(e^{-\frac{M}{N}})$

这相当于 Get 开销乘以一个上面的值。当然对 range 来说优化不大。

#### checksums

`ReadOptions::verify_checksums` 表示读是否要验证 checksum, `Options::paranoid_checks` 在设置之后，可以检查打开的数据库的错误。

`leveldb::RepairDB` 会尽可能修复数据。

## Read Path


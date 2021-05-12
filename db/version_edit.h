// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_VERSION_EDIT_H_
#define STORAGE_LEVELDB_DB_VERSION_EDIT_H_

#include <set>
#include <utility>
#include <vector>

#include "db/dbformat.h"

namespace leveldb {

class VersionSet;

// LevelDB 有 Table 和 TableCache, 同时也有 BlockCache, 但是对上层而言，似乎提供是 FileMetaData.
// 其中别的字段都很好理解，比较恶心的是 `allowed_seeks`
//
// refs 是 Version 对它的引用, Version 析构的时候, 会减少这个引用. 这部分内容是 replay 出来的。
// TODO(mwish): 搞清楚 FileMetaData 生命周期
struct FileMetaData {
  FileMetaData() : refs(0), allowed_seeks(1 << 30), file_size(0) {}

  int refs;
  int allowed_seeks;  // Seeks allowed until compaction
  uint64_t number;
  uint64_t file_size;    // File size in bytes
  InternalKey smallest;  // Smallest internal key served by table
  InternalKey largest;   // Largest internal key served by table
};

// 这个 edit 有日志文件 Id, 文件 id
class VersionEdit {
 public:
  VersionEdit() { Clear(); }
  ~VersionEdit() = default;

  void Clear();

  void SetComparatorName(const Slice& name) {
    has_comparator_ = true;
    comparator_ = name.ToString();
  }

  // 只有 MemCompaction 的时候，会设置 log number, 推高水位.
  // 理论上正常应该最多也只要 recover 两个 log 文件.
  // 因为 CURRENT 写成功后，切 mem 的时候会 Set, 然后这中间最多两个。
  void SetLogNumber(uint64_t num) {
    has_log_number_ = true;
    log_number_ = num;
  }

  void SetPrevLogNumber(uint64_t num) {
    has_prev_log_number_ = true;
    prev_log_number_ = num;
  }
  // 空余的文件 id.
  void SetNextFile(uint64_t num) {
    has_next_file_number_ = true;
    next_file_number_ = num;
  }
  // sequence_id
  // 在 `VersionSet::LogAndApply` 的时候, VersionEdit 会被设置 last_sequence.
  void SetLastSequence(SequenceNumber seq) {
    has_last_sequence_ = true;
    last_sequence_ = seq;
  }

  // 在 Compact 完成后, 对于 Size Compaction, 需要正确设置文件变动后的 Compaction Pointer.
  void SetCompactPointer(int level, const InternalKey& key) {
    compact_pointers_.push_back(std::make_pair(level, key));
  }

  // * 所以实际上 AddFile 的时候，是没有 allow_seeks 的
  // Add the specified file at the specified number.
  // REQUIRES: This version has not been saved (see VersionSet::SaveTo)
  // REQUIRES: "smallest" and "largest" are smallest and largest keys in file
  void AddFile(int level, uint64_t file, uint64_t file_size,
               const InternalKey& smallest, const InternalKey& largest) {
    FileMetaData f;
    f.number = file;
    f.file_size = file_size;
    f.smallest = smallest;
    f.largest = largest;
    new_files_.push_back(std::make_pair(level, f));
  }

  // deleted_files_ 是不能 remove 的，所以仍然需要记录一下。
  // Note: deleted_files_ 只是用来 Apply 的时候把对应内容删掉，和 debug 的时候使用.
  // 本身不会删除磁盘上的文件.
  //
  // Delete the specified "file" from the specified "level".
  void RemoveFile(int level, uint64_t file) {
    deleted_files_.insert(std::make_pair(level, file));
  }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(const Slice& src);

  std::string DebugString() const;

 private:
  friend class VersionSet;

  typedef std::set<std::pair<int, uint64_t>> DeletedFileSet;

  // 这个是用户自定义的 comparator, 需要持久化
  std::string comparator_;
  // 写入的最后一条 LSN. 在 MemTable Compaction 的时候推高.
  uint64_t log_number_;
  // 上个 Version 的 log_number_.
  uint64_t prev_log_number_;
  uint64_t next_file_number_;
  SequenceNumber last_sequence_;
  bool has_comparator_;
  bool has_log_number_;
  bool has_prev_log_number_;
  bool has_next_file_number_;
  bool has_last_sequence_;

  // (level, message)
  std::vector<std::pair<int, InternalKey>> compact_pointers_;
  DeletedFileSet deleted_files_;
  std::vector<std::pair<int, FileMetaData>> new_files_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_VERSION_EDIT_H_

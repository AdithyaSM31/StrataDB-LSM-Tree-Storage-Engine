#ifndef STRATA_TABLE_TABLE_BUILDER_H_
#define STRATA_TABLE_TABLE_BUILDER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "strata/env.h"
#include "strata/options.h"
#include "strata/status.h"
#include "table/block_builder.h"
#include "table/format.h"
#include "util/bloom.h"
#include "util/comparator.h"

namespace strata {

// Writes an SSTable (see table/format.h for the layout) from entries added in
// sorted order.
//
// Keys are internal keys. The bloom filter is built from their USER keys, so a
// Get that searches with a different sequence number still matches.
//
// The builder only appends to `file`. It does not Sync or Close it: the caller
// (memtable flush or compaction) must Sync and Close the file, then sync the
// directory, before the table is recorded anywhere or trusted after a crash.
class TableBuilder {
 public:
  // `comparator` orders the internal keys (an InternalKeyComparator). It and
  // `file` must outlive the builder. Uses options.block_size,
  // options.block_restart_interval and options.bloom_bits_per_key (0 = no filter).
  TableBuilder(const Options& options, const Comparator* comparator, WritableFile* file);
  ~TableBuilder();

  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  // REQUIRES: key is an internal key, greater than every key added so far.
  // REQUIRES: Finish() and Abandon() have not been called.
  void Add(std::string_view key, std::string_view value);

  // Writes the filter, metaindex, index and footer. After this, FileSize() is
  // the table's final size.
  Status Finish();

  // Stops building without finishing the file (for example after an error).
  // The caller should delete the partial file.
  void Abandon();

  // The first error from writing, or OK. Once an error occurs, later Add()s
  // are ignored and Finish() returns it.
  Status status() const { return status_; }

  uint64_t NumEntries() const { return num_entries_; }

  // Bytes written so far; after Finish(), the size of the finished table.
  uint64_t FileSize() const { return offset_; }

 private:
  // Ends the current data block and adds its index entry.
  void FlushDataBlock();
  void WriteBlock(BlockBuilder* block, BlockHandle* handle);
  void WriteRawBlock(std::string_view contents, CompressionType type, BlockHandle* handle);

  const Options options_;
  const Comparator* const comparator_;
  WritableFile* const file_;
  uint64_t offset_ = 0;
  Status status_;

  BlockBuilder data_block_;
  BlockBuilder index_block_;
  std::unique_ptr<BloomFilterPolicy> filter_policy_;  // null if filtering is off
  std::vector<std::string> filter_keys_;              // distinct user keys, in order

  std::string last_key_;
  uint64_t num_entries_ = 0;
  bool closed_ = false;  // Finish() or Abandon() was called
};

}  // namespace strata

#endif  // STRATA_TABLE_TABLE_BUILDER_H_

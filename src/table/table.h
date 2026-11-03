#ifndef STRATA_TABLE_TABLE_H_
#define STRATA_TABLE_TABLE_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "strata/env.h"
#include "strata/iterator.h"
#include "strata/options.h"
#include "strata/status.h"
#include "table/block.h"
#include "table/format.h"
#include "util/bloom.h"
#include "util/cache.h"
#include "util/comparator.h"

namespace strata {

using BlockCache = ShardedLRUCache<Block>;

// A read-only SSTable. After Open(), the index block and the bloom filter are
// held in memory; each lookup reads at most one data block from the file.
//
// Thread safety: all methods are const and safe to call from several threads
// at once (file reads use pread).
class Table {
 public:
  // Opens the table stored in `file`, which is `file_size` bytes long.
  // `comparator` orders the internal keys (an InternalKeyComparator) and must
  // outlive the table. Returns Corruption if the footer or index block is bad.
  // A missing or damaged filter is not an error: the table is simply read
  // without one, because the filter only ever saves reads.
  // `block_cache` is nullable; if provided, uncompressed data blocks are
  // cached in it.
  static Status Open(const Options& options, const Comparator* comparator,
                     std::unique_ptr<RandomAccessFile> file, uint64_t file_size,
                     std::unique_ptr<Table>* table, BlockCache* block_cache = nullptr);

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;
  ~Table();

  // Iterates over every entry (internal key -> value). The table must outlive
  // the iterator. Errors reading data blocks are reported by status().
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) const;
  std::unique_ptr<Iterator> NewIterator() const;

  // Point lookup for the read path. Finds the first entry whose internal key
  // is >= `internal_key` and copies it into *key and *value, setting *found.
  //
  // *found is false if the bloom filter rules out the user key, or if no such
  // entry exists. When *found is true the entry may still belong to a
  // different (larger) user key, or be a tombstone: the caller checks the user
  // key and the type, exactly as MemTable::Get does.
  Status InternalGet(const ReadOptions& options, std::string_view internal_key, bool* found,
                     std::string* key, std::string* value) const;
  Status InternalGet(std::string_view internal_key, bool* found, std::string* key,
                     std::string* value) const;

  // True if the table has a usable bloom filter (for tests and stats).
  bool has_filter() const { return filter_.has_value(); }

 private:
  Table(const Comparator* comparator, std::unique_ptr<RandomAccessFile> file, uint64_t file_size,
        std::unique_ptr<Block> index_block, BlockCache* block_cache, uint64_t cache_id);

  // Loads the bloom filter named in the metaindex block, if there is a usable one.
  void ReadFilter(const BlockHandle& metaindex_handle);

  // Reads the data block named by an index value and returns an iterator that
  // owns it, or an error iterator.
  std::unique_ptr<Iterator> BlockReader(const ReadOptions& options,
                                        std::string_view index_value) const;

  const Comparator* const comparator_;
  const std::unique_ptr<RandomAccessFile> file_;
  const uint64_t file_size_;
  const std::unique_ptr<Block> index_block_;
  BloomFilterPolicy filter_policy_;    // used only to read; k comes from the filter
  std::optional<std::string> filter_;  // the filter block, if usable
  BlockCache* const block_cache_;
  const uint64_t cache_id_;
};

}  // namespace strata

#endif  // STRATA_TABLE_TABLE_H_

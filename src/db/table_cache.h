#ifndef STRATA_DB_TABLE_CACHE_H_
#define STRATA_DB_TABLE_CACHE_H_

#include <cstdint>
#include <memory>
#include <string>

#include "db/dbformat.h"
#include "strata/env.h"
#include "strata/options.h"
#include "strata/status.h"
#include "table/table.h"
#include "util/cache.h"

namespace strata {

// Thread-safe cache of open SSTable files (bounded by max_open_files - 10).
class TableCache {
 public:
  TableCache(std::string dbname, const Options& options, const InternalKeyComparator* icmp,
             BlockCache* block_cache = nullptr);
  ~TableCache() = default;

  TableCache(const TableCache&) = delete;
  TableCache& operator=(const TableCache&) = delete;

  // Opens (or reuses) the table for this file number.
  Status FindTable(uint64_t number, uint64_t file_size, std::shared_ptr<Table>* table);

  // Opens (or reuses) the table and returns an iterator over its contents.
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options, uint64_t number,
                                        uint64_t file_size, Table** tableptr = nullptr);
  std::unique_ptr<Iterator> NewIterator(uint64_t number, uint64_t file_size,
                                        Table** tableptr = nullptr);

  // Erases the table for this file number from the cache when the file is deleted.
  void Erase(uint64_t number);

  // Backward compatibility alias for Erase.
  void Evict(uint64_t number) { Erase(number); }

  // Total open tables currently in cache.
  size_t TotalCharge() const { return cache_->TotalCharge(); }

 private:
  static std::string TableKey(uint64_t number);

  Env* const env_;
  const std::string dbname_;
  const Options options_;
  const InternalKeyComparator* const icmp_;
  BlockCache* const block_cache_;

  std::unique_ptr<ShardedLRUCache<Table>> cache_;
};

}  // namespace strata

#endif  // STRATA_DB_TABLE_CACHE_H_

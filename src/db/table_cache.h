#ifndef STRATA_DB_TABLE_CACHE_H_
#define STRATA_DB_TABLE_CACHE_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "db/dbformat.h"
#include "strata/env.h"
#include "strata/options.h"
#include "strata/status.h"
#include "table/table.h"

namespace strata {

class TableCache {
 public:
  TableCache(std::string dbname, const Options& options, const InternalKeyComparator* icmp);
  ~TableCache() = default;

  TableCache(const TableCache&) = delete;
  TableCache& operator=(const TableCache&) = delete;

  // Opens (or reuses) the table for this file number.
  Status FindTable(uint64_t number, uint64_t file_size, std::shared_ptr<Table>* table);

  // Evicts the table for this file number when the file is deleted.
  void Evict(uint64_t number);

 private:
  Env* const env_;
  const std::string dbname_;
  const Options options_;
  const InternalKeyComparator* const icmp_;

  std::mutex mutex_;
  std::unordered_map<uint64_t, std::shared_ptr<Table>> cache_;
};

}  // namespace strata

#endif  // STRATA_DB_TABLE_CACHE_H_

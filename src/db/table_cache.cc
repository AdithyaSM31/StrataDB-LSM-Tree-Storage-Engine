#include "db/table_cache.h"

#include <memory>
#include <utility>

#include "db/filename.h"

namespace strata {

TableCache::TableCache(std::string dbname, const Options& options,
                       const InternalKeyComparator* icmp)
    : env_(options.env == nullptr ? Env::Default() : options.env),
      dbname_(std::move(dbname)),
      options_(options),
      icmp_(icmp) {}

Status TableCache::FindTable(uint64_t number, uint64_t file_size, std::shared_ptr<Table>* table) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = cache_.find(number);
  if (it != cache_.end()) {
    *table = it->second;
    return Status::OK();
  }

  const std::string fname = TableFileName(dbname_, number);
  if (file_size == 0) {
    Status s = env_->GetFileSize(fname, &file_size);
    if (!s.ok()) {
      return s;
    }
  }

  std::unique_ptr<RandomAccessFile> file;
  Status s = env_->NewRandomAccessFile(fname, &file);
  if (!s.ok()) {
    return s;
  }

  std::unique_ptr<Table> t;
  s = Table::Open(options_, icmp_, std::move(file), file_size, &t);
  if (!s.ok()) {
    return s;
  }

  std::shared_ptr<Table> st = std::move(t);
  cache_[number] = st;
  *table = std::move(st);
  return Status::OK();
}

void TableCache::Evict(uint64_t number) {
  std::lock_guard<std::mutex> lock(mutex_);
  cache_.erase(number);
}

}  // namespace strata

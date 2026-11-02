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
  if (table == nullptr) {
    return Status::InvalidArgument("table is null");
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(number);
    if (it != cache_.end()) {
      *table = it->second;
      return Status::OK();
    }
  }

  // Open outside the lock: it reads the footer, index block and filter from
  // disk, and lookups in tables that are already open should not wait for it.
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
  std::unique_ptr<Table> opened;
  s = Table::Open(options_, icmp_, std::move(file), file_size, &opened);
  if (!s.ok()) {
    return s;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  // Another thread may have opened the same table meanwhile; keep the first so
  // every reader shares one Table.
  auto [it, inserted] = cache_.emplace(number, std::shared_ptr<Table>(std::move(opened)));
  (void)inserted;
  *table = it->second;
  return Status::OK();
}

void TableCache::Evict(uint64_t number) {
  std::lock_guard<std::mutex> lock(mutex_);
  cache_.erase(number);
}

namespace {

class TableIteratorWrapper : public Iterator {
 public:
  TableIteratorWrapper(std::shared_ptr<Table> table, std::unique_ptr<Iterator> iter)
      : table_(std::move(table)), iter_(std::move(iter)) {}
  ~TableIteratorWrapper() override = default;

  bool Valid() const override { return iter_->Valid(); }
  void SeekToFirst() override { iter_->SeekToFirst(); }
  void SeekToLast() override { iter_->SeekToLast(); }
  void Seek(std::string_view target) override { iter_->Seek(target); }
  void Next() override { iter_->Next(); }
  void Prev() override { iter_->Prev(); }
  std::string_view key() const override { return iter_->key(); }
  std::string_view value() const override { return iter_->value(); }
  Status status() const override { return iter_->status(); }

 private:
  std::shared_ptr<Table> table_;
  std::unique_ptr<Iterator> iter_;
};

}  // namespace

std::unique_ptr<Iterator> TableCache::NewIterator(uint64_t number, uint64_t file_size,
                                                  Table** tableptr) {
  std::shared_ptr<Table> table;
  Status s = FindTable(number, file_size, &table);
  if (!s.ok()) {
    return NewErrorIterator(s);
  }
  if (tableptr != nullptr) {
    *tableptr = table.get();
  }
  return std::make_unique<TableIteratorWrapper>(table, table->NewIterator());
}

}  // namespace strata

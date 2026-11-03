#include "db/table_cache.h"

#include <memory>
#include <utility>

#include "db/filename.h"
#include "util/coding.h"

namespace strata {

std::string TableCache::TableKey(uint64_t number) {
  std::string key;
  PutFixed64(&key, number);
  return key;
}

TableCache::TableCache(std::string dbname, const Options& options,
                       const InternalKeyComparator* icmp, BlockCache* block_cache)
    : env_(options.env == nullptr ? Env::Default() : options.env),
      dbname_(std::move(dbname)),
      options_(options),
      icmp_(icmp),
      block_cache_(block_cache),
      cache_(std::make_unique<ShardedLRUCache<Table>>(
          options_.max_open_files > 10 ? static_cast<size_t>(options_.max_open_files - 10) : 0)) {}

Status TableCache::FindTable(uint64_t number, uint64_t file_size, std::shared_ptr<Table>* table) {
  if (table == nullptr) {
    return Status::InvalidArgument("table is null");
  }
  const std::string key = TableKey(number);
  *table = cache_->Lookup(key);
  if (*table != nullptr) {
    return Status::OK();
  }

  // Open outside the cache: it reads the footer, index block and filter from
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
  s = Table::Open(options_, icmp_, std::move(file), file_size, &opened, block_cache_);
  if (!s.ok()) {
    return s;
  }

  // Another thread may have opened the same table meanwhile; keep the first so
  // every reader shares one Table.
  *table = cache_->Insert(key, std::shared_ptr<Table>(std::move(opened)), 1);
  return Status::OK();
}

void TableCache::Erase(uint64_t number) { cache_->Erase(TableKey(number)); }

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

std::unique_ptr<Iterator> TableCache::NewIterator(const ReadOptions& options, uint64_t number,
                                                  uint64_t file_size, Table** tableptr) {
  std::shared_ptr<Table> table;
  Status s = FindTable(number, file_size, &table);
  if (!s.ok()) {
    return NewErrorIterator(s);
  }
  if (tableptr != nullptr) {
    *tableptr = table.get();
  }
  return std::make_unique<TableIteratorWrapper>(table, table->NewIterator(options));
}

std::unique_ptr<Iterator> TableCache::NewIterator(uint64_t number, uint64_t file_size,
                                                  Table** tableptr) {
  return NewIterator(ReadOptions(), number, file_size, tableptr);
}

}  // namespace strata

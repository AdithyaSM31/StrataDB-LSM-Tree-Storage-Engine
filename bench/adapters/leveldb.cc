#include <memory>
#include <string>
#include <string_view>

#include "bench/adapter.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif

#include "leveldb/cache.h"
#include "leveldb/db.h"
#include "leveldb/filter_policy.h"
#include "leveldb/options.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace strata::bench {

namespace {

Status ToStrataStatus(const leveldb::Status& s) {
  if (s.ok()) {
    return Status::OK();
  }
  if (s.IsNotFound()) {
    return Status::NotFound(s.ToString());
  }
  if (s.IsCorruption()) {
    return Status::Corruption(s.ToString());
  }
  if (s.IsIOError()) {
    return Status::IOError(s.ToString());
  }
  if (s.IsInvalidArgument()) {
    return Status::InvalidArgument(s.ToString());
  }
  if (s.IsNotSupportedError()) {
    return Status::NotSupported(s.ToString());
  }
  return Status::IOError(s.ToString());
}

}  // namespace

class LevelDBAdapter : public BenchmarkAdapter {
 public:
  LevelDBAdapter() = default;
  ~LevelDBAdapter() override { Close(); }

  Status Open(const std::string& dbname, const BenchmarkOptions& options) override {
    Close();
    dbname_ = dbname;
    leveldb::Options l_opts;
    l_opts.create_if_missing = options.create_if_missing;
    l_opts.error_if_exists = options.error_if_exists;
    l_opts.write_buffer_size = options.write_buffer_size;
    l_opts.block_size = options.block_size;
    l_opts.max_open_files = options.max_open_files;

    // CRITICAL FAIRNESS RULE:
    // StrataDB does not compress; disable LevelDB compression so comparison is fair.
    l_opts.compression = leveldb::kNoCompression;

    if (options.block_cache_size > 0) {
      cache_ = leveldb::NewLRUCache(options.block_cache_size);
      l_opts.block_cache = cache_;
    }
    if (options.bloom_filter_bits > 0) {
      filter_policy_ = leveldb::NewBloomFilterPolicy(options.bloom_filter_bits);
      l_opts.filter_policy = filter_policy_;
    }

    leveldb::DB* raw_db = nullptr;
    leveldb::Status s = leveldb::DB::Open(l_opts, dbname, &raw_db);
    if (!s.ok()) {
      delete cache_;
      cache_ = nullptr;
      delete filter_policy_;
      filter_policy_ = nullptr;
      return ToStrataStatus(s);
    }
    db_ = raw_db;
    return Status::OK();
  }

  void Close() override {
    delete db_;
    db_ = nullptr;
    delete cache_;
    cache_ = nullptr;
    delete filter_policy_;
    filter_policy_ = nullptr;
  }

  Status Put(std::string_view key, std::string_view value) override {
    leveldb::WriteOptions opts;
    opts.sync = false;
    return ToStrataStatus(db_->Put(opts, leveldb::Slice(key.data(), key.size()),
                                   leveldb::Slice(value.data(), value.size())));
  }

  Status Delete(std::string_view key) override {
    leveldb::WriteOptions opts;
    opts.sync = false;
    return ToStrataStatus(db_->Delete(opts, leveldb::Slice(key.data(), key.size())));
  }

  Status Get(std::string_view key, std::string* value) override {
    leveldb::ReadOptions opts;
    opts.fill_cache = true;
    return ToStrataStatus(db_->Get(opts, leveldb::Slice(key.data(), key.size()), value));
  }

  Status SyncPut(std::string_view key, std::string_view value) override {
    leveldb::WriteOptions opts;
    opts.sync = true;
    return ToStrataStatus(db_->Put(opts, leveldb::Slice(key.data(), key.size()),
                                   leveldb::Slice(value.data(), value.size())));
  }

  Status Seek(std::string_view key) override {
    leveldb::ReadOptions opts;
    opts.fill_cache = true;
    std::unique_ptr<leveldb::Iterator> iter(db_->NewIterator(opts));
    iter->Seek(leveldb::Slice(key.data(), key.size()));
    return ToStrataStatus(iter->status());
  }

  Status Scan(std::string_view start_key, size_t limit, size_t* count) override {
    leveldb::ReadOptions opts;
    opts.fill_cache = true;
    std::unique_ptr<leveldb::Iterator> iter(db_->NewIterator(opts));
    iter->Seek(leveldb::Slice(start_key.data(), start_key.size()));
    size_t c = 0;
    while (iter->Valid() && c < limit) {
      ++c;
      iter->Next();
    }
    if (count != nullptr) {
      *count = c;
    }
    return ToStrataStatus(iter->status());
  }

  std::string Name() const override { return "LevelDB"; }

  std::string GetProperty(std::string_view prop) override {
    if (!db_) {
      return "";
    }
    std::string val;
    if (db_->GetProperty(leveldb::Slice(prop.data(), prop.size()), &val)) {
      return val;
    }
    return "";
  }

 private:
  std::string dbname_;
  leveldb::DB* db_ = nullptr;
  leveldb::Cache* cache_ = nullptr;
  const leveldb::FilterPolicy* filter_policy_ = nullptr;
};

std::unique_ptr<BenchmarkAdapter> CreateLevelDBAdapter() {
  return std::make_unique<LevelDBAdapter>();
}

}  // namespace strata::bench

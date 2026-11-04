#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "bench/adapter.h"
#include "strata/db.h"
#include "strata/options.h"
#include "strata/status.h"

namespace strata::bench {

class StrataAdapter : public BenchmarkAdapter {
 public:
  StrataAdapter() = default;
  ~StrataAdapter() override { Close(); }

  Status Open(const std::string& dbname, const BenchmarkOptions& options) override {
    Close();
    dbname_ = dbname;
    strata::Options s_opts;
    s_opts.create_if_missing = options.create_if_missing;
    s_opts.error_if_exists = options.error_if_exists;
    s_opts.write_buffer_size = options.write_buffer_size;
    s_opts.block_size = options.block_size;
    s_opts.block_cache_capacity = options.block_cache_size;
    s_opts.bloom_bits_per_key = options.bloom_filter_bits;
    s_opts.max_open_files = options.max_open_files;

    return DB::Open(s_opts, dbname, &db_);
  }

  void Close() override { db_.reset(); }

  Status Put(std::string_view key, std::string_view value) override {
    if (!db_) {
      return Status::InvalidArgument("DB is not open");
    }
    WriteOptions opts;
    opts.sync = false;
    return db_->Put(opts, key, value);
  }

  Status Delete(std::string_view key) override {
    if (!db_) {
      return Status::InvalidArgument("DB is not open");
    }
    WriteOptions opts;
    opts.sync = false;
    return db_->Delete(opts, key);
  }

  Status Get(std::string_view key, std::string* value) override {
    if (!db_) {
      return Status::InvalidArgument("DB is not open");
    }
    ReadOptions opts;
    opts.fill_cache = true;
    return db_->Get(opts, key, value);
  }

  Status SyncPut(std::string_view key, std::string_view value) override {
    if (!db_) {
      return Status::InvalidArgument("DB is not open");
    }
    WriteOptions opts;
    opts.sync = true;
    return db_->Put(opts, key, value);
  }

  Status Seek(std::string_view key) override {
    if (!db_) {
      return Status::InvalidArgument("DB is not open");
    }
    ReadOptions opts;
    opts.fill_cache = true;
    auto iter = db_->NewIterator(opts);
    iter->Seek(key);
    return iter->status();
  }

  Status Scan(std::string_view start_key, size_t limit, size_t* count) override {
    if (!db_) {
      return Status::InvalidArgument("DB is not open");
    }
    ReadOptions opts;
    opts.fill_cache = true;
    auto iter = db_->NewIterator(opts);
    iter->Seek(start_key);
    size_t c = 0;
    while (iter->Valid() && c < limit) {
      ++c;
      iter->Next();
    }
    if (count != nullptr) {
      *count = c;
    }
    return iter->status();
  }

  std::string Name() const override { return "StrataDB"; }

  std::string GetProperty(std::string_view prop) override {
    if (!db_) {
      return "";
    }
    std::string val;
    if (db_->GetProperty(prop, &val)) {
      return val;
    }
    return "";
  }

 private:
  std::string dbname_;
  std::unique_ptr<DB> db_;
};

std::unique_ptr<BenchmarkAdapter> CreateStrataAdapter() {
  return std::make_unique<StrataAdapter>();
}

}  // namespace strata::bench

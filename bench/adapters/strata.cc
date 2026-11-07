#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bench/adapter.h"
#include "strata/db.h"
#include "strata/iterator.h"
#include "strata/options.h"
#include "strata/status.h"

namespace strata::bench {

namespace {

class StrataCursor : public BenchmarkCursor {
 public:
  explicit StrataCursor(std::unique_ptr<Iterator> iter) : iter_(std::move(iter)) {}

  void Seek(std::string_view target) override { iter_->Seek(target); }
  void Next() override { iter_->Next(); }
  bool Valid() const override { return iter_->Valid(); }
  std::string_view key() const override { return iter_->key(); }
  std::string_view value() const override { return iter_->value(); }
  Status status() const override { return iter_->status(); }

 private:
  std::unique_ptr<Iterator> iter_;
};

}  // namespace

class StrataAdapter final : public BenchmarkAdapter {
 public:
  StrataAdapter() = default;
  ~StrataAdapter() override { db_.reset(); }

  Status Open(const std::string& dbname, const BenchmarkOptions& options) override {
    Close();
    Options s_opts;
    s_opts.create_if_missing = options.create_if_missing;
    s_opts.error_if_exists = options.error_if_exists;
    s_opts.write_buffer_size = options.write_buffer_size;
    s_opts.block_size = options.block_size;
    s_opts.block_cache_capacity = options.block_cache_size;
    s_opts.bloom_bits_per_key = options.bloom_filter_bits;
    s_opts.max_open_files = options.max_open_files;
    s_opts.group_commit = options.group_commit;
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
    return db_->Get(ReadOptions(), key, value);
  }

  Status SyncPut(std::string_view key, std::string_view value) override {
    if (!db_) {
      return Status::InvalidArgument("DB is not open");
    }
    WriteOptions opts;
    opts.sync = true;
    return db_->Put(opts, key, value);
  }

  std::unique_ptr<BenchmarkCursor> NewCursor() override {
    if (!db_) {
      return nullptr;
    }
    return std::make_unique<StrataCursor>(db_->NewIterator(ReadOptions()));
  }

  std::string Name() const override { return "StrataDB"; }

  std::string Stats() override {
    std::string stats;
    if (!db_ || !db_->GetProperty("strata.stats", &stats)) {
      return "";
    }
    return stats;
  }

  Status LevelShapes(std::vector<LevelShape>* shapes) override {
    shapes->clear();
    const std::string stats = Stats();
    if (stats.empty()) {
      return Status::InvalidArgument("strata.stats is not available");
    }
    // Per-level rows look like "  1       5       9.91       6 ...".
    std::istringstream lines(stats);
    std::string line;
    while (std::getline(lines, line)) {
      int level = 0;
      unsigned long long files = 0;
      double megabytes = 0.0;
      if (std::sscanf(line.c_str(), "%d %llu %lf", &level, &files, &megabytes) == 3) {
        shapes->push_back({files, static_cast<uint64_t>(megabytes * 1048576.0)});
      }
    }
    if (shapes->empty()) {
      return Status::Corruption("could not parse strata.stats");
    }
    return Status::OK();
  }

  Status BlockCacheStats(uint64_t* hits, uint64_t* misses) override {
    std::string hit_count;
    std::string miss_count;
    if (!db_ || !db_->GetProperty("strata.block-cache-hits", &hit_count) ||
        !db_->GetProperty("strata.block-cache-misses", &miss_count)) {
      return Status::InvalidArgument("block cache properties are not available");
    }
    *hits = std::strtoull(hit_count.c_str(), nullptr, 10);
    *misses = std::strtoull(miss_count.c_str(), nullptr, 10);
    return Status::OK();
  }

 private:
  std::unique_ptr<DB> db_;
};

std::unique_ptr<BenchmarkAdapter> CreateStrataAdapter() {
  return std::make_unique<StrataAdapter>();
}

}  // namespace strata::bench

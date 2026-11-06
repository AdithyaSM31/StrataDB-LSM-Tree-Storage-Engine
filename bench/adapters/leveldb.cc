#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bench/adapter.h"
#include "leveldb/cache.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/filter_policy.h"
#include "leveldb/iterator.h"
#include "leveldb/options.h"

namespace strata::bench {

namespace {

// Conversion runs inside the timed section. NotFound is the expected result of
// every readmissing lookup, so it must not spend time building a message that
// the StrataDB path doesn't build.
Status ToStrataStatus(const leveldb::Status& s) {
  if (s.ok()) {
    return Status::OK();
  }
  if (s.IsNotFound()) {
    return Status::NotFound("");
  }
  if (s.IsCorruption()) {
    return Status::Corruption(s.ToString());
  }
  if (s.IsInvalidArgument()) {
    return Status::InvalidArgument(s.ToString());
  }
  if (s.IsNotSupportedError()) {
    return Status::NotSupported(s.ToString());
  }
  return Status::IOError(s.ToString());
}

leveldb::Slice ToSlice(std::string_view s) { return {s.data(), s.size()}; }

// StrataDB verifies the checksum of every block it reads and has no way to turn
// that off, so LevelDB must do the same work on the same reads.
leveldb::ReadOptions VerifyingReadOptions() {
  leveldb::ReadOptions opts;
  opts.verify_checksums = true;
  return opts;
}

class PreadFile final : public leveldb::RandomAccessFile {
 public:
  PreadFile(int fd, std::string name) : fd_(fd), name_(std::move(name)) {}
  ~PreadFile() override { ::close(fd_); }

  leveldb::Status Read(uint64_t offset, size_t n, leveldb::Slice* result,
                       char* scratch) const override {
    const ssize_t bytes = ::pread(fd_, scratch, n, static_cast<off_t>(offset));
    if (bytes < 0) {
      *result = leveldb::Slice(scratch, 0);
      return leveldb::Status::IOError(name_, std::strerror(errno));
    }
    *result = leveldb::Slice(scratch, static_cast<size_t>(bytes));
    return leveldb::Status::OK();
  }

 private:
  const int fd_;
  const std::string name_;
};

// Returning data in the caller's scratch buffer is what makes LevelDB keep index
// and filter blocks on its heap and put data blocks in its block cache.
class NoMmapEnv final : public leveldb::EnvWrapper {
 public:
  NoMmapEnv() : EnvWrapper(leveldb::Env::Default()) {}

  leveldb::Status NewRandomAccessFile(const std::string& fname,
                                      leveldb::RandomAccessFile** result) override {
    *result = nullptr;
    const int fd = ::open(fname.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      return leveldb::Status::IOError(fname, std::strerror(errno));
    }
    *result = new PreadFile(fd, fname);
    return leveldb::Status::OK();
  }
};

class LevelDBCursor : public BenchmarkCursor {
 public:
  explicit LevelDBCursor(leveldb::Iterator* iter) : iter_(iter) {}

  void Seek(std::string_view target) override { iter_->Seek(ToSlice(target)); }
  void Next() override { iter_->Next(); }
  bool Valid() const override { return iter_->Valid(); }
  std::string_view key() const override {
    const leveldb::Slice k = iter_->key();
    return {k.data(), k.size()};
  }
  std::string_view value() const override {
    const leveldb::Slice v = iter_->value();
    return {v.data(), v.size()};
  }
  Status status() const override { return ToStrataStatus(iter_->status()); }

 private:
  std::unique_ptr<leveldb::Iterator> iter_;
};

}  // namespace

class LevelDBAdapter final : public BenchmarkAdapter {
 public:
  explicit LevelDBAdapter(bool use_mmap) {
    if (!use_mmap) {
      no_mmap_env_ = std::make_unique<NoMmapEnv>();
    }
  }
  ~LevelDBAdapter() override { CloseDatabase(); }

  Status Open(const std::string& dbname, const BenchmarkOptions& options) override {
    Close();
    leveldb::Options l_opts;
    l_opts.create_if_missing = options.create_if_missing;
    l_opts.error_if_exists = options.error_if_exists;
    l_opts.write_buffer_size = options.write_buffer_size;
    l_opts.block_size = options.block_size;
    l_opts.max_open_files = options.max_open_files;
    if (no_mmap_env_ != nullptr) {
      l_opts.env = no_mmap_env_.get();
    }

    // StrataDB does not compress.
    l_opts.compression = leveldb::kNoCompression;

    // Makes compactions verify the blocks they read, as StrataDB's do.
    l_opts.paranoid_checks = true;

    // Always pass a cache: with none, LevelDB creates its own 8 MB cache, while
    // capacity 0 really does turn caching off.
    cache_.reset(leveldb::NewLRUCache(options.block_cache_size));
    l_opts.block_cache = cache_.get();

    if (options.bloom_filter_bits > 0) {
      filter_policy_.reset(leveldb::NewBloomFilterPolicy(options.bloom_filter_bits));
      l_opts.filter_policy = filter_policy_.get();
    }

    leveldb::DB* raw_db = nullptr;
    const leveldb::Status s = leveldb::DB::Open(l_opts, dbname, &raw_db);
    if (!s.ok()) {
      cache_.reset();
      filter_policy_.reset();
      return ToStrataStatus(s);
    }
    db_.reset(raw_db);
    return Status::OK();
  }

  void Close() override { CloseDatabase(); }

  Status Put(std::string_view key, std::string_view value) override {
    if (db_ == nullptr) {
      return Status::InvalidArgument("DB is not open");
    }
    leveldb::WriteOptions opts;
    opts.sync = false;
    return ToStrataStatus(db_->Put(opts, ToSlice(key), ToSlice(value)));
  }

  Status Delete(std::string_view key) override {
    if (db_ == nullptr) {
      return Status::InvalidArgument("DB is not open");
    }
    leveldb::WriteOptions opts;
    opts.sync = false;
    return ToStrataStatus(db_->Delete(opts, ToSlice(key)));
  }

  Status Get(std::string_view key, std::string* value) override {
    if (db_ == nullptr) {
      return Status::InvalidArgument("DB is not open");
    }
    return ToStrataStatus(db_->Get(VerifyingReadOptions(), ToSlice(key), value));
  }

  Status SyncPut(std::string_view key, std::string_view value) override {
    if (db_ == nullptr) {
      return Status::InvalidArgument("DB is not open");
    }
    leveldb::WriteOptions opts;
    opts.sync = true;
    return ToStrataStatus(db_->Put(opts, ToSlice(key), ToSlice(value)));
  }

  std::unique_ptr<BenchmarkCursor> NewCursor() override {
    if (db_ == nullptr) {
      return nullptr;
    }
    return std::make_unique<LevelDBCursor>(db_->NewIterator(VerifyingReadOptions()));
  }

  std::string Name() const override {
    return no_mmap_env_ != nullptr ? "LevelDB-nommap" : "LevelDB";
  }

  std::string Stats() override {
    std::string stats;
    if (db_ == nullptr || !db_->GetProperty("leveldb.stats", &stats)) {
      return "";
    }
    return stats;
  }

  Status LevelShapes(std::vector<LevelShape>* shapes) override {
    shapes->clear();
    if (db_ == nullptr) {
      return Status::InvalidArgument("DB is not open");
    }
    std::string sstables;
    if (!db_->GetProperty("leveldb.sstables", &sstables)) {
      return Status::NotSupported("leveldb.sstables is not available");
    }
    // Sections look like "--- level 1 ---" followed by " 17:2097152['a' .. 'd']".
    std::istringstream lines(sstables);
    std::string line;
    while (std::getline(lines, line)) {
      int level = 0;
      unsigned long long number = 0;
      unsigned long long size = 0;
      if (std::sscanf(line.c_str(), "--- level %d ---", &level) == 1) {
        shapes->emplace_back();
      } else if (std::sscanf(line.c_str(), " %llu:%llu[", &number, &size) == 2 &&
                 !shapes->empty()) {
        shapes->back().files += 1;
        shapes->back().bytes += size;
      }
    }
    if (shapes->empty()) {
      return Status::Corruption("could not parse leveldb.sstables");
    }
    return Status::OK();
  }

 private:
  // The database uses the cache and the filter policy, so it must go first.
  void CloseDatabase() {
    db_.reset();
    cache_.reset();
    filter_policy_.reset();
  }

  std::unique_ptr<NoMmapEnv> no_mmap_env_;  // null when LevelDB uses its default mmap reads
  std::unique_ptr<const leveldb::FilterPolicy> filter_policy_;
  std::unique_ptr<leveldb::Cache> cache_;
  std::unique_ptr<leveldb::DB> db_;
};

std::unique_ptr<BenchmarkAdapter> CreateLevelDBAdapter(bool use_mmap) {
  return std::make_unique<LevelDBAdapter>(use_mmap);
}

}  // namespace strata::bench

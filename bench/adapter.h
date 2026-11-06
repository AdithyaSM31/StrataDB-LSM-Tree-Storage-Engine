#ifndef STRATA_BENCH_ADAPTER_H_
#define STRATA_BENCH_ADAPTER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "strata/status.h"

namespace strata::bench {

struct BenchmarkOptions {
  size_t write_buffer_size = size_t{4} * 1024 * 1024;
  size_t block_size = 4096;
  size_t block_cache_size = size_t{8} * 1024 * 1024;  // 0 turns the block cache off
  int bloom_filter_bits = 10;                         // 0 writes no filters
  int max_open_files = 1000;
  bool create_if_missing = true;
  bool error_if_exists = false;
};

struct LevelShape {
  uint64_t files = 0;
  uint64_t bytes = 0;

  bool operator==(const LevelShape&) const = default;
};

// A position in the database. Must not outlive the adapter's open database.
class BenchmarkCursor {
 public:
  virtual ~BenchmarkCursor() = default;

  virtual void Seek(std::string_view target) = 0;
  virtual void Next() = 0;
  virtual bool Valid() const = 0;
  // REQUIRES: Valid(). Valid until the cursor next moves.
  virtual std::string_view key() const = 0;
  virtual std::string_view value() const = 0;
  virtual Status status() const = 0;
};

class BenchmarkAdapter {
 public:
  virtual ~BenchmarkAdapter() = default;

  virtual Status Open(const std::string& dbname, const BenchmarkOptions& options) = 0;
  virtual void Close() = 0;
  virtual Status Put(std::string_view key, std::string_view value) = 0;
  virtual Status Delete(std::string_view key) = 0;
  virtual Status Get(std::string_view key, std::string* value) = 0;
  virtual Status SyncPut(std::string_view key, std::string_view value) = 0;
  // Returns nullptr if the database is not open.
  virtual std::unique_ptr<BenchmarkCursor> NewCursor() = 0;
  virtual std::string Name() const = 0;
  // The engine's own statistics report; empty if it has none.
  virtual std::string Stats() = 0;
  // Replaces *shapes with the files and bytes at each level, L0 first.
  virtual Status LevelShapes(std::vector<LevelShape>* shapes) = 0;
};

std::unique_ptr<BenchmarkAdapter> CreateStrataAdapter();
// LevelDB maps its table files by default, so its index and filter blocks stay in
// the page cache and its data blocks never enter its block cache. With use_mmap
// false it reads with pread, as StrataDB does, and reports its name as
// "LevelDB-nommap". Both use the same files on disk.
std::unique_ptr<BenchmarkAdapter> CreateLevelDBAdapter(bool use_mmap);

}  // namespace strata::bench

#endif  // STRATA_BENCH_ADAPTER_H_

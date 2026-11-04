#ifndef STRATA_BENCH_ADAPTER_H_
#define STRATA_BENCH_ADAPTER_H_

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "strata/status.h"

namespace strata::bench {

struct BenchmarkOptions {
  size_t write_buffer_size = 4 * 1024 * 1024;  // 4MB
  size_t block_size = 4096;                    // 4KB
  size_t block_cache_size = 8 * 1024 * 1024;   // 8MB
  int bloom_filter_bits = 10;                  // 10 bits per key
  int max_open_files = 1000;
  bool create_if_missing = true;
  bool error_if_exists = false;
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
  virtual Status Seek(std::string_view key) = 0;
  virtual Status Scan(std::string_view start_key, size_t limit, size_t* count) = 0;
  virtual std::string Name() const = 0;
  virtual std::string GetProperty(std::string_view prop) = 0;
};

std::unique_ptr<BenchmarkAdapter> CreateStrataAdapter();
std::unique_ptr<BenchmarkAdapter> CreateLevelDBAdapter();

}  // namespace strata::bench

#endif  // STRATA_BENCH_ADAPTER_H_

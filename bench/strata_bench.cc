#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "bench/adapter.h"
#include "util/histogram.h"

namespace strata::bench {

namespace {

constexpr uint64_t kDefaultSeed = 133742;

struct BenchConfig {
  std::string engine = "both";
  std::string benchmarks = "all";
  size_t num = 1000000;
  size_t sync_num = 0;  // 0 means auto: min(num, 1000)
  int threads = 1;
  std::string db_dir = "./strata_bench_data";
  size_t write_buffer_size = 4 * 1024 * 1024;
  size_t block_size = 4096;
  size_t block_cache_size = 8 * 1024 * 1024;
  int bloom_filter_bits = 10;
  int max_open_files = 1000;
  size_t value_size = 100;
  bool stats = false;
  std::string csv_path = "bench/results/benchmarks.csv";
};

struct BenchmarkResult {
  std::string engine;
  std::string workload;
  int threads = 1;
  size_t num_ops = 0;
  double elapsed_seconds = 0.0;
  double ops_per_sec = 0.0;
  double mb_per_sec = 0.0;
  double p50_us = 0.0;
  double p99_us = 0.0;
  double p999_us = 0.0;
  double avg_us = 0.0;
  double min_us = 0.0;
  double max_us = 0.0;
};

std::string FormatKey(uint64_t k) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%016" PRIu64, k);
  return std::string(buf, 16);
}

class ValueGenerator {
 public:
  explicit ValueGenerator(size_t max_val_size = 100) {
    size_t sz = std::max(size_t{1024 * 1024}, max_val_size * 2 + 1024);
    std::mt19937 rng(42);
    buffer_.resize(sz);
    for (size_t i = 0; i < buffer_.size(); ++i) {
      buffer_[i] = static_cast<char>('a' + (rng() % 26));
    }
  }

  std::string Generate(uint64_t key, size_t size) const {
    if (size == 0) return "";
    size_t span = buffer_.size() - size;
    size_t offset = (span > 0) ? ((key * 97) % span) : 0;
    return buffer_.substr(offset, size);
  }

 private:
  std::string buffer_;
};

size_t ParseSize(std::string_view val) {
  if (val.empty()) return 0;
  std::string s(val);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  if (s.empty()) return 0;

  size_t multiplier = 1;
  char last = static_cast<char>(std::tolower(static_cast<unsigned char>(s.back())));
  if (last == 'b' && s.size() > 1) {
    s.pop_back();
    last = static_cast<char>(std::tolower(static_cast<unsigned char>(s.back())));
  }
  if (last == 'k') {
    multiplier = 1024ULL;
    s.pop_back();
  } else if (last == 'm') {
    multiplier = 1024ULL * 1024ULL;
    s.pop_back();
  } else if (last == 'g') {
    multiplier = 1024ULL * 1024ULL * 1024ULL;
    s.pop_back();
  }
  try {
    return std::stoull(s) * multiplier;
  } catch (...) {
    return 0;
  }
}

std::vector<std::string> Split(std::string_view str, char delim) {
  std::vector<std::string> result;
  std::string s(str);
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, delim)) {
    if (!item.empty()) {
      result.push_back(item);
    }
  }
  return result;
}

void PrintConfig(const BenchConfig& cfg) {
  std::cout << "================================================================================\n"
            << "StrataDB Benchmark Harness\n"
            << "================================================================================\n"
            << "Engines:        " << cfg.engine << "\n"
            << "Benchmarks:     " << cfg.benchmarks << "\n"
            << "Entries:        " << cfg.num << "\n"
            << "Value Size:     " << cfg.value_size << " bytes\n"
            << "Key Size:       16 bytes\n"
            << "Threads:        " << cfg.threads << "\n"
            << "Write Buffer:   " << (cfg.write_buffer_size / (1024.0 * 1024.0)) << " MB\n"
            << "Block Size:     " << cfg.block_size << " bytes\n"
            << "Block Cache:    " << (cfg.block_cache_size / (1024.0 * 1024.0)) << " MB\n"
            << "Bloom Filter:   " << cfg.bloom_filter_bits << " bits/key\n"
            << "Max Open Files: " << cfg.max_open_files << "\n"
            << "Compression:    none (fairness enforced across both engines)\n"
            << "DB Directory:   " << cfg.db_dir << "\n"
            << "CSV Output:     " << cfg.csv_path << "\n"
            << "================================================================================\n"
            << std::endl;
}

void PrintTableHeader() {
  std::cout << "-----------------------------------------------------------------------------------"
               "--------------------------\n";
  std::printf("%-10s  %-18s  %7s  %12s  %9s  %9s  %9s  %9s\n", "Engine", "Workload", "Threads",
              "Ops/sec", "MB/s", "p50 (us)", "p99 (us)", "p99.9 (us)");
  std::cout << "-----------------------------------------------------------------------------------"
               "--------------------------\n";
}

void PrintTableRow(const BenchmarkResult& r) {
  std::printf("%-10s  %-18s  %7d  %12.1f  %9.2f  %9.2f  %9.2f  %9.2f\n", r.engine.c_str(),
              r.workload.c_str(), r.threads, r.ops_per_sec, r.mb_per_sec, r.p50_us, r.p99_us,
              r.p999_us);
  std::fflush(stdout);
}

void AppendCsv(const std::string& csv_path, const std::vector<BenchmarkResult>& results) {
  std::filesystem::path path(csv_path);
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }

  bool file_exists = std::filesystem::exists(path);
  std::ofstream csv(csv_path, std::ios::app);
  if (!csv.is_open()) {
    std::fprintf(stderr, "Failed to open CSV output file: %s\n", csv_path.c_str());
    return;
  }

  if (!file_exists || std::filesystem::file_size(path) == 0) {
    csv << "engine,workload,threads,num_ops,total_sec,ops_per_sec,mb_per_sec,p50_us,p99_us,p999_us,"
           "avg_us,min_us,max_us\n";
  }

  for (const auto& r : results) {
    csv << r.engine << "," << r.workload << "," << r.threads << "," << r.num_ops << ","
        << std::fixed << std::setprecision(4) << r.elapsed_seconds << "," << std::setprecision(1)
        << r.ops_per_sec << "," << std::setprecision(2) << r.mb_per_sec << "," << r.p50_us << ","
        << r.p99_us << "," << r.p999_us << "," << r.avg_us << "," << r.min_us << "," << r.max_us
        << "\n";
  }
}

class BenchmarkRunner {
 public:
  BenchmarkRunner(const BenchConfig& cfg, std::unique_ptr<BenchmarkAdapter> adapter)
      : cfg_(cfg), adapter_(std::move(adapter)), val_gen_(cfg.value_size) {
    engine_name_ = adapter_->Name();
    engine_db_dir_ = cfg_.db_dir + "/" + (engine_name_ == "StrataDB" ? "strata" : "leveldb");
  }

  std::vector<BenchmarkResult> RunWorkloads(const std::vector<std::string>& workloads) {
    std::vector<BenchmarkResult> results;
    for (const auto& w : workloads) {
      BenchmarkResult r = RunSingleWorkload(w);
      if (r.num_ops > 0) {
        PrintTableRow(r);
        results.push_back(r);
      }
    }
    if (cfg_.stats) {
      std::string stats_prop = (engine_name_ == "StrataDB") ? "strata.stats" : "leveldb.stats";
      std::string stats_val = adapter_->GetProperty(stats_prop);
      if (!stats_val.empty()) {
        std::cout << "\n--- " << engine_name_ << " Stats ---\n" << stats_val << "\n";
      }
    }
    adapter_->Close();
    return results;
  }

 private:
  BenchmarkOptions MakeOptions(bool create_if_missing, bool error_if_exists = false) {
    BenchmarkOptions opts;
    opts.write_buffer_size = cfg_.write_buffer_size;
    opts.block_size = cfg_.block_size;
    opts.block_cache_size = cfg_.block_cache_size;
    opts.bloom_filter_bits = cfg_.bloom_filter_bits;
    opts.max_open_files = cfg_.max_open_files;
    opts.create_if_missing = create_if_missing;
    opts.error_if_exists = error_if_exists;
    return opts;
  }

  void MarkPopulated(size_t n) {
    std::string meta_path = engine_db_dir_ + "/.populated_num";
    std::ofstream meta(meta_path);
    if (meta.is_open()) {
      meta << n;
    }
  }

  void ReopenFreshDB() {
    adapter_->Close();
    std::filesystem::remove_all(engine_db_dir_);
    std::filesystem::create_directories(engine_db_dir_);
    Status s = adapter_->Open(engine_db_dir_, MakeOptions(true, false));
    if (!s.ok()) {
      std::fprintf(stderr, "Failed to open fresh db at %s: %s\n", engine_db_dir_.c_str(),
                   s.ToString().c_str());
    }
  }

  void EnsureDBOpen() {
    if (!std::filesystem::exists(engine_db_dir_)) {
      ReopenFreshDB();
      return;
    }
    Status s = adapter_->Open(engine_db_dir_, MakeOptions(true, false));
    if (!s.ok()) {
      ReopenFreshDB();
    }
  }

  void PopulateForReadsIfNeeded() {
    bool needs_populate = true;
    std::string meta_path = engine_db_dir_ + "/.populated_num";
    if (std::filesystem::exists(engine_db_dir_) && std::filesystem::exists(meta_path)) {
      std::ifstream meta(meta_path);
      size_t pop_num = 0;
      if (meta >> pop_num && pop_num == cfg_.num) {
        needs_populate = false;
      }
    }
    if (needs_populate) {
      ReopenFreshDB();
      RunFillRandom(false);  // Silent pre-population
      MarkPopulated(cfg_.num);
    } else {
      EnsureDBOpen();
    }
  }

  BenchmarkResult RunSingleWorkload(const std::string& workload) {
    if (workload == "fillseq") {
      ReopenFreshDB();
      return RunFillSeq();
    } else if (workload == "fillrandom") {
      ReopenFreshDB();
      return RunFillRandom(true);
    } else if (workload == "overwrite") {
      PopulateForReadsIfNeeded();
      return RunOverwrite();
    } else if (workload == "fillsync") {
      ReopenFreshDB();
      return RunFillSync();
    } else if (workload == "readseq") {
      PopulateForReadsIfNeeded();
      return RunReadSeq();
    } else if (workload == "readrandom") {
      PopulateForReadsIfNeeded();
      return RunReadRandom();
    } else if (workload == "readmissing") {
      PopulateForReadsIfNeeded();
      return RunReadMissing();
    } else if (workload == "seekrandom") {
      PopulateForReadsIfNeeded();
      return RunSeekRandom();
    } else if (workload == "scan") {
      PopulateForReadsIfNeeded();
      return RunScan();
    } else if (workload == "readwhilewriting") {
      PopulateForReadsIfNeeded();
      return RunReadWhileWriting();
    } else {
      std::fprintf(stderr, "Unknown workload: %s\n", workload.c_str());
      return BenchmarkResult{};
    }
  }

  BenchmarkResult RunFillSeq() {
    const size_t n = cfg_.num;
    const int t_count = cfg_.threads;
    std::vector<Histogram> histograms(t_count);
    std::vector<std::thread> workers;
    workers.reserve(t_count);

    std::atomic<bool> start_signal{false};
    std::atomic<int> ready_count{0};
    std::atomic<size_t> error_count{0};

    for (int t = 0; t < t_count; ++t) {
      workers.emplace_back(
          [this, t, t_count, n, &histograms, &start_signal, &ready_count, &error_count]() {
            size_t start_key = t * (n / t_count);
            size_t end_key = (t == t_count - 1) ? n : (t + 1) * (n / t_count);

            ready_count.fetch_add(1);
            while (!start_signal.load(std::memory_order_acquire)) {
            }

            for (size_t k = start_key; k < end_key; ++k) {
              std::string key = FormatKey(k);
              std::string val = val_gen_.Generate(k, cfg_.value_size);

              auto t0 = std::chrono::steady_clock::now();
              Status s = adapter_->Put(key, val);
              auto t1 = std::chrono::steady_clock::now();

              if (!s.ok()) {
                if (error_count.fetch_add(1) < 5) {
                  std::fprintf(stderr, "Put error: %s\n", s.ToString().c_str());
                }
              }
              double micros = std::chrono::duration<double, std::micro>(t1 - t0).count();
              histograms[t].Add(micros);
            }
          });
    }

    while (ready_count.load() < t_count) {
      std::this_thread::yield();
    }
    auto t_start = std::chrono::steady_clock::now();
    start_signal.store(true, std::memory_order_release);

    for (auto& w : workers) {
      w.join();
    }
    auto t_end = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t_end - t_start).count();

    MarkPopulated(n);
    return BuildResult("fillseq", t_count, n, elapsed, histograms, (16 + cfg_.value_size) * n);
  }

  BenchmarkResult RunFillRandom(bool record) {
    const size_t n = cfg_.num;
    const int t_count = cfg_.threads;
    std::vector<Histogram> histograms(t_count);
    std::vector<std::thread> workers;
    workers.reserve(t_count);

    // Pre-generate keys per thread for deterministic identical sequence across engines
    std::vector<std::vector<uint64_t>> thread_keys(t_count);
    for (int t = 0; t < t_count; ++t) {
      size_t start_key = t * (n / t_count);
      size_t end_key = (t == t_count - 1) ? n : (t + 1) * (n / t_count);
      thread_keys[t].resize(end_key - start_key);
      std::iota(thread_keys[t].begin(), thread_keys[t].end(), start_key);
      std::mt19937_64 rng(kDefaultSeed + static_cast<uint64_t>(t));
      std::shuffle(thread_keys[t].begin(), thread_keys[t].end(), rng);
    }

    std::atomic<bool> start_signal{false};
    std::atomic<int> ready_count{0};
    std::atomic<size_t> error_count{0};

    for (int t = 0; t < t_count; ++t) {
      workers.emplace_back(
          [this, t, &thread_keys, &histograms, &start_signal, &ready_count, &error_count]() {
            const auto& keys = thread_keys[t];
            ready_count.fetch_add(1);
            while (!start_signal.load(std::memory_order_acquire)) {
            }

            for (uint64_t k : keys) {
              std::string key = FormatKey(k);
              std::string val = val_gen_.Generate(k, cfg_.value_size);

              auto t0 = std::chrono::steady_clock::now();
              Status s = adapter_->Put(key, val);
              auto t1 = std::chrono::steady_clock::now();

              if (!s.ok()) {
                if (error_count.fetch_add(1) < 5) {
                  std::fprintf(stderr, "Put error: %s\n", s.ToString().c_str());
                }
              }
              double micros = std::chrono::duration<double, std::micro>(t1 - t0).count();
              histograms[t].Add(micros);
            }
          });
    }

    while (ready_count.load() < t_count) {
      std::this_thread::yield();
    }
    auto t_start = std::chrono::steady_clock::now();
    start_signal.store(true, std::memory_order_release);

    for (auto& w : workers) {
      w.join();
    }
    auto t_end = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t_end - t_start).count();

    MarkPopulated(n);
    if (!record) {
      return BenchmarkResult{};
    }
    return BuildResult("fillrandom", t_count, n, elapsed, histograms, (16 + cfg_.value_size) * n);
  }

  BenchmarkResult RunOverwrite() {
    const size_t n = cfg_.num;
    const int t_count = cfg_.threads;
    std::vector<Histogram> histograms(t_count);
    std::vector<std::thread> workers;
    workers.reserve(t_count);

    std::vector<std::vector<uint64_t>> thread_keys(t_count);
    for (int t = 0; t < t_count; ++t) {
      size_t start_key = t * (n / t_count);
      size_t end_key = (t == t_count - 1) ? n : (t + 1) * (n / t_count);
      thread_keys[t].resize(end_key - start_key);
      std::iota(thread_keys[t].begin(), thread_keys[t].end(), start_key);
      std::mt19937_64 rng(kDefaultSeed + 1000 + static_cast<uint64_t>(t));
      std::shuffle(thread_keys[t].begin(), thread_keys[t].end(), rng);
    }

    std::atomic<bool> start_signal{false};
    std::atomic<int> ready_count{0};
    std::atomic<size_t> error_count{0};

    for (int t = 0; t < t_count; ++t) {
      workers.emplace_back(
          [this, t, &thread_keys, &histograms, &start_signal, &ready_count, &error_count]() {
            const auto& keys = thread_keys[t];
            ready_count.fetch_add(1);
            while (!start_signal.load(std::memory_order_acquire)) {
            }

            for (uint64_t k : keys) {
              std::string key = FormatKey(k);
              std::string val = val_gen_.Generate(k + 9999, cfg_.value_size);

              auto t0 = std::chrono::steady_clock::now();
              Status s = adapter_->Put(key, val);
              auto t1 = std::chrono::steady_clock::now();

              if (!s.ok()) {
                if (error_count.fetch_add(1) < 5) {
                  std::fprintf(stderr, "Overwrite error: %s\n", s.ToString().c_str());
                }
              }
              double micros = std::chrono::duration<double, std::micro>(t1 - t0).count();
              histograms[t].Add(micros);
            }
          });
    }

    while (ready_count.load() < t_count) {
      std::this_thread::yield();
    }
    auto t_start = std::chrono::steady_clock::now();
    start_signal.store(true, std::memory_order_release);

    for (auto& w : workers) {
      w.join();
    }
    auto t_end = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t_end - t_start).count();

    return BuildResult("overwrite", t_count, n, elapsed, histograms, (16 + cfg_.value_size) * n);
  }

  BenchmarkResult RunFillSync() {
    size_t sync_n = cfg_.sync_num > 0 ? cfg_.sync_num : std::min(cfg_.num, size_t{1000});
    const int t_count = cfg_.threads;
    std::vector<Histogram> histograms(t_count);
    std::vector<std::thread> workers;
    workers.reserve(t_count);

    std::atomic<bool> start_signal{false};
    std::atomic<int> ready_count{0};
    std::atomic<size_t> error_count{0};

    for (int t = 0; t < t_count; ++t) {
      workers.emplace_back(
          [this, t, t_count, sync_n, &histograms, &start_signal, &ready_count, &error_count]() {
            size_t start_key = t * (sync_n / t_count);
            size_t end_key = (t == t_count - 1) ? sync_n : (t + 1) * (sync_n / t_count);

            ready_count.fetch_add(1);
            while (!start_signal.load(std::memory_order_acquire)) {
            }

            for (size_t k = start_key; k < end_key; ++k) {
              std::string key = FormatKey(k);
              std::string val = val_gen_.Generate(k, cfg_.value_size);

              auto t0 = std::chrono::steady_clock::now();
              Status s = adapter_->SyncPut(key, val);
              auto t1 = std::chrono::steady_clock::now();

              if (!s.ok()) {
                if (error_count.fetch_add(1) < 5) {
                  std::fprintf(stderr, "SyncPut error: %s\n", s.ToString().c_str());
                }
              }
              double micros = std::chrono::duration<double, std::micro>(t1 - t0).count();
              histograms[t].Add(micros);
            }
          });
    }

    while (ready_count.load() < t_count) {
      std::this_thread::yield();
    }
    auto t_start = std::chrono::steady_clock::now();
    start_signal.store(true, std::memory_order_release);

    for (auto& w : workers) {
      w.join();
    }
    auto t_end = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t_end - t_start).count();

    std::error_code ec;
    std::filesystem::remove(engine_db_dir_ + "/.populated_num", ec);
    return BuildResult("fillsync", t_count, sync_n, elapsed, histograms,
                       (16 + cfg_.value_size) * sync_n);
  }

  BenchmarkResult RunReadSeq() {
    const size_t n = cfg_.num;
    const int t_count = cfg_.threads;
    std::vector<Histogram> histograms(t_count);
    std::vector<std::thread> workers;
    workers.reserve(t_count);

    std::atomic<bool> start_signal{false};
    std::atomic<int> ready_count{0};
    std::atomic<size_t> error_count{0};

    for (int t = 0; t < t_count; ++t) {
      workers.emplace_back(
          [this, t, t_count, n, &histograms, &start_signal, &ready_count, &error_count]() {
            size_t start_key = t * (n / t_count);
            size_t end_key = (t == t_count - 1) ? n : (t + 1) * (n / t_count);
            std::string val;

            ready_count.fetch_add(1);
            while (!start_signal.load(std::memory_order_acquire)) {
            }

            for (size_t k = start_key; k < end_key; ++k) {
              std::string key = FormatKey(k);

              auto t0 = std::chrono::steady_clock::now();
              Status s = adapter_->Get(key, &val);
              auto t1 = std::chrono::steady_clock::now();

              if (!s.ok()) {
                if (error_count.fetch_add(1) < 5) {
                  std::fprintf(stderr, "Get error: %s\n", s.ToString().c_str());
                }
              }
              double micros = std::chrono::duration<double, std::micro>(t1 - t0).count();
              histograms[t].Add(micros);
            }
          });
    }

    while (ready_count.load() < t_count) {
      std::this_thread::yield();
    }
    auto t_start = std::chrono::steady_clock::now();
    start_signal.store(true, std::memory_order_release);

    for (auto& w : workers) {
      w.join();
    }
    auto t_end = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t_end - t_start).count();

    return BuildResult("readseq", t_count, n, elapsed, histograms, (16 + cfg_.value_size) * n);
  }

  BenchmarkResult RunReadRandom() {
    const size_t n = cfg_.num;
    const int t_count = cfg_.threads;
    std::vector<Histogram> histograms(t_count);
    std::vector<std::thread> workers;
    workers.reserve(t_count);

    std::atomic<bool> start_signal{false};
    std::atomic<int> ready_count{0};
    std::atomic<size_t> error_count{0};

    for (int t = 0; t < t_count; ++t) {
      workers.emplace_back(
          [this, t, t_count, n, &histograms, &start_signal, &ready_count, &error_count]() {
            size_t start_op = t * (n / t_count);
            size_t end_op = (t == t_count - 1) ? n : (t + 1) * (n / t_count);
            size_t ops = end_op - start_op;
            std::mt19937_64 rng(kDefaultSeed + 2000 + static_cast<uint64_t>(t));
            std::string val;

            ready_count.fetch_add(1);
            while (!start_signal.load(std::memory_order_acquire)) {
            }

            for (size_t i = 0; i < ops; ++i) {
              uint64_t k = rng() % n;
              std::string key = FormatKey(k);

              auto t0 = std::chrono::steady_clock::now();
              Status s = adapter_->Get(key, &val);
              auto t1 = std::chrono::steady_clock::now();

              if (!s.ok()) {
                if (error_count.fetch_add(1) < 5) {
                  std::fprintf(stderr, "ReadRandom error: %s\n", s.ToString().c_str());
                }
              }
              double micros = std::chrono::duration<double, std::micro>(t1 - t0).count();
              histograms[t].Add(micros);
            }
          });
    }

    while (ready_count.load() < t_count) {
      std::this_thread::yield();
    }
    auto t_start = std::chrono::steady_clock::now();
    start_signal.store(true, std::memory_order_release);

    for (auto& w : workers) {
      w.join();
    }
    auto t_end = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t_end - t_start).count();

    return BuildResult("readrandom", t_count, n, elapsed, histograms, (16 + cfg_.value_size) * n);
  }

  BenchmarkResult RunReadMissing() {
    const size_t n = cfg_.num;
    const int t_count = cfg_.threads;
    std::vector<Histogram> histograms(t_count);
    std::vector<std::thread> workers;
    workers.reserve(t_count);

    std::atomic<bool> start_signal{false};
    std::atomic<int> ready_count{0};

    for (int t = 0; t < t_count; ++t) {
      workers.emplace_back([this, t, t_count, n, &histograms, &start_signal, &ready_count]() {
        size_t start_op = t * (n / t_count);
        size_t end_op = (t == t_count - 1) ? n : (t + 1) * (n / t_count);
        size_t ops = end_op - start_op;
        std::mt19937_64 rng(kDefaultSeed + 3000 + static_cast<uint64_t>(t));
        std::string val;

        ready_count.fetch_add(1);
        while (!start_signal.load(std::memory_order_acquire)) {
        }

        for (size_t i = 0; i < ops; ++i) {
          uint64_t k = n + (rng() % n);
          std::string key = FormatKey(k);

          auto t0 = std::chrono::steady_clock::now();
          Status s = adapter_->Get(key, &val);
          auto t1 = std::chrono::steady_clock::now();

          (void)s;  // Expect NotFound
          double micros = std::chrono::duration<double, std::micro>(t1 - t0).count();
          histograms[t].Add(micros);
        }
      });
    }

    while (ready_count.load() < t_count) {
      std::this_thread::yield();
    }
    auto t_start = std::chrono::steady_clock::now();
    start_signal.store(true, std::memory_order_release);

    for (auto& w : workers) {
      w.join();
    }
    auto t_end = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t_end - t_start).count();

    return BuildResult("readmissing", t_count, n, elapsed, histograms, 16 * n);
  }

  BenchmarkResult RunSeekRandom() {
    const size_t n = cfg_.num;
    const int t_count = cfg_.threads;
    std::vector<Histogram> histograms(t_count);
    std::vector<std::thread> workers;
    workers.reserve(t_count);

    std::atomic<bool> start_signal{false};
    std::atomic<int> ready_count{0};
    std::atomic<size_t> error_count{0};

    for (int t = 0; t < t_count; ++t) {
      workers.emplace_back(
          [this, t, t_count, n, &histograms, &start_signal, &ready_count, &error_count]() {
            size_t start_op = t * (n / t_count);
            size_t end_op = (t == t_count - 1) ? n : (t + 1) * (n / t_count);
            size_t ops = end_op - start_op;
            std::mt19937_64 rng(kDefaultSeed + 4000 + static_cast<uint64_t>(t));

            ready_count.fetch_add(1);
            while (!start_signal.load(std::memory_order_acquire)) {
            }

            for (size_t i = 0; i < ops; ++i) {
              uint64_t k = rng() % n;
              std::string key = FormatKey(k);

              auto t0 = std::chrono::steady_clock::now();
              Status s = adapter_->Seek(key);
              auto t1 = std::chrono::steady_clock::now();

              if (!s.ok()) {
                if (error_count.fetch_add(1) < 5) {
                  std::fprintf(stderr, "Seek error: %s\n", s.ToString().c_str());
                }
              }
              double micros = std::chrono::duration<double, std::micro>(t1 - t0).count();
              histograms[t].Add(micros);
            }
          });
    }

    while (ready_count.load() < t_count) {
      std::this_thread::yield();
    }
    auto t_start = std::chrono::steady_clock::now();
    start_signal.store(true, std::memory_order_release);

    for (auto& w : workers) {
      w.join();
    }
    auto t_end = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t_end - t_start).count();

    return BuildResult("seekrandom", t_count, n, elapsed, histograms, 16 * n);
  }

  BenchmarkResult RunScan() {
    const size_t n = cfg_.num;
    const int t_count = cfg_.threads;
    const size_t kScanLen = 100;
    std::vector<Histogram> histograms(t_count);
    std::vector<std::thread> workers;
    workers.reserve(t_count);

    std::atomic<bool> start_signal{false};
    std::atomic<int> ready_count{0};
    std::atomic<size_t> error_count{0};

    for (int t = 0; t < t_count; ++t) {
      workers.emplace_back(
          [this, t, t_count, n, &histograms, &start_signal, &ready_count, &error_count]() {
            size_t start_op = t * (n / t_count);
            size_t end_op = (t == t_count - 1) ? n : (t + 1) * (n / t_count);
            size_t ops = end_op - start_op;
            std::mt19937_64 rng(kDefaultSeed + 6000 + static_cast<uint64_t>(t));

            ready_count.fetch_add(1);
            while (!start_signal.load(std::memory_order_acquire)) {
            }

            for (size_t i = 0; i < ops; ++i) {
              uint64_t k = (n > kScanLen) ? (rng() % (n - kScanLen)) : 0;
              std::string start_key = FormatKey(k);
              size_t count = 0;

              auto t0 = std::chrono::steady_clock::now();
              Status s = adapter_->Scan(start_key, kScanLen, &count);
              auto t1 = std::chrono::steady_clock::now();

              if (!s.ok()) {
                if (error_count.fetch_add(1) < 5) {
                  std::fprintf(stderr, "Scan error: %s\n", s.ToString().c_str());
                }
              }
              double micros = std::chrono::duration<double, std::micro>(t1 - t0).count();
              histograms[t].Add(micros);
            }
          });
    }

    while (ready_count.load() < t_count) {
      std::this_thread::yield();
    }
    auto t_start = std::chrono::steady_clock::now();
    start_signal.store(true, std::memory_order_release);

    for (auto& w : workers) {
      w.join();
    }
    auto t_end = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t_end - t_start).count();

    return BuildResult("scan", t_count, n, elapsed, histograms,
                       (16 + cfg_.value_size) * kScanLen * (n / 10));
  }

  BenchmarkResult RunReadWhileWriting() {
    const size_t n = cfg_.num;
    const int reader_threads = cfg_.threads;
    std::vector<Histogram> read_histograms(reader_threads);
    std::vector<std::thread> readers;
    readers.reserve(reader_threads);

    std::atomic<bool> start_signal{false};
    std::atomic<bool> stop_writer{false};
    std::atomic<int> ready_count{0};

    // Background writer thread
    std::thread writer([this, n, &start_signal, &stop_writer, &ready_count]() {
      ready_count.fetch_add(1);
      while (!start_signal.load(std::memory_order_acquire)) {
      }

      uint64_t k = n;
      while (!stop_writer.load(std::memory_order_relaxed)) {
        std::string key = FormatKey(k);
        std::string val = val_gen_.Generate(k, cfg_.value_size);
        (void)adapter_->Put(key, val);
        ++k;
      }
    });

    for (int t = 0; t < reader_threads; ++t) {
      readers.emplace_back(
          [this, t, reader_threads, n, &read_histograms, &start_signal, &ready_count]() {
            size_t start_op = t * (n / reader_threads);
            size_t end_op = (t == reader_threads - 1) ? n : (t + 1) * (n / reader_threads);
            size_t ops = end_op - start_op;
            std::mt19937_64 rng(kDefaultSeed + 5000 + static_cast<uint64_t>(t));
            std::string val;

            ready_count.fetch_add(1);
            while (!start_signal.load(std::memory_order_acquire)) {
            }

            for (size_t i = 0; i < ops; ++i) {
              uint64_t k = rng() % n;
              std::string key = FormatKey(k);

              auto t0 = std::chrono::steady_clock::now();
              Status s = adapter_->Get(key, &val);
              auto t1 = std::chrono::steady_clock::now();

              (void)s;
              double micros = std::chrono::duration<double, std::micro>(t1 - t0).count();
              read_histograms[t].Add(micros);
            }
          });
    }

    while (ready_count.load() < reader_threads + 1) {
      std::this_thread::yield();
    }
    auto t_start = std::chrono::steady_clock::now();
    start_signal.store(true, std::memory_order_release);

    for (auto& r : readers) {
      r.join();
    }
    stop_writer.store(true, std::memory_order_relaxed);
    writer.join();

    auto t_end = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t_end - t_start).count();

    std::error_code ec;
    std::filesystem::remove(engine_db_dir_ + "/.populated_num", ec);

    return BuildResult("readwhilewriting", reader_threads, n, elapsed, read_histograms,
                       (16 + cfg_.value_size) * n);
  }

  BenchmarkResult BuildResult(const std::string& workload, int threads, size_t ops, double elapsed,
                              const std::vector<Histogram>& histograms, size_t total_bytes) {
    Histogram merged;
    for (const auto& h : histograms) {
      merged.Merge(h);
    }

    BenchmarkResult r;
    r.engine = engine_name_;
    r.workload = workload;
    r.threads = threads;
    r.num_ops = ops;
    r.elapsed_seconds = elapsed;
    r.ops_per_sec = (elapsed > 0.0) ? (static_cast<double>(ops) / elapsed) : 0.0;
    r.mb_per_sec =
        (elapsed > 0.0) ? (static_cast<double>(total_bytes) / (elapsed * 1048576.0)) : 0.0;
    r.p50_us = merged.Percentile(50.0);
    r.p99_us = merged.Percentile(99.0);
    r.p999_us = merged.Percentile(99.9);
    r.avg_us = merged.Average();
    r.min_us = merged.Min();
    r.max_us = merged.Max();
    return r;
  }

  BenchConfig cfg_;
  std::unique_ptr<BenchmarkAdapter> adapter_;
  ValueGenerator val_gen_;
  std::string engine_name_;
  std::string engine_db_dir_;
};

}  // namespace

}  // namespace strata::bench

int main(int argc, char* argv[]) {
  using namespace strata::bench;

  BenchConfig cfg;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.rfind("--engine=", 0) == 0) {
      cfg.engine = arg.substr(9);
    } else if (arg.rfind("--benchmarks=", 0) == 0) {
      cfg.benchmarks = arg.substr(13);
    } else if (arg.rfind("--num=", 0) == 0) {
      cfg.num = std::stoull(arg.substr(6));
    } else if (arg.rfind("--sync_num=", 0) == 0) {
      cfg.sync_num = std::stoull(arg.substr(11));
    } else if (arg.rfind("--threads=", 0) == 0) {
      cfg.threads = std::clamp(std::stoi(arg.substr(10)), 1, 64);
    } else if (arg.rfind("--db=", 0) == 0) {
      cfg.db_dir = arg.substr(5);
    } else if (arg.rfind("--write_buffer_size=", 0) == 0) {
      cfg.write_buffer_size = ParseSize(arg.substr(20));
    } else if (arg.rfind("--cache_size=", 0) == 0) {
      cfg.block_cache_size = ParseSize(arg.substr(13));
    } else if (arg.rfind("--block_size=", 0) == 0) {
      cfg.block_size = ParseSize(arg.substr(13));
    } else if (arg.rfind("--bloom_bits=", 0) == 0) {
      cfg.bloom_filter_bits = std::stoi(arg.substr(13));
    } else if (arg.rfind("--max_open_files=", 0) == 0) {
      cfg.max_open_files = std::stoi(arg.substr(17));
    } else if (arg.rfind("--value_size=", 0) == 0) {
      cfg.value_size = std::stoull(arg.substr(13));
    } else if (arg.rfind("--csv=", 0) == 0) {
      cfg.csv_path = arg.substr(6);
    } else if (arg == "--stats" || arg == "--stats=1" || arg == "--stats=true") {
      cfg.stats = true;
    } else if (arg == "--help" || arg == "-h") {
      std::cout
          << "Usage: strata_bench [options]\n"
          << "  --engine=strata,leveldb,both     Target database engine (default: both)\n"
          << "  --benchmarks=fillseq,readseq,... Workload list or 'all' (default: all)\n"
          << "  --num=N                          Number of key/value pairs (default: 1000000)\n"
          << "  --sync_num=N                     Number of sync writes for fillsync (default: "
             "min(num, 1000))\n"
          << "  --threads=N                      Number of worker threads (default: 1)\n"
          << "  --db=PATH                        Database directory (default: "
             "./strata_bench_data)\n"
          << "  --write_buffer_size=N            Write buffer size in bytes or KB/MB (default: "
             "4MB)\n"
          << "  --cache_size=N                   Block cache size in bytes or KB/MB (default: "
             "8MB)\n"
          << "  --block_size=N                   Block size in bytes (default: 4096)\n"
          << "  --bloom_bits=N                   Bloom filter bits per key (default: 10)\n"
          << "  --max_open_files=N               Max open files (default: 1000)\n"
          << "  --value_size=N                   Size of values in bytes (default: 100)\n"
          << "  --stats                          Print engine-specific internal stats after run\n"
          << "  --csv=PATH                       CSV output file (default: "
             "bench/results/benchmarks.csv)\n";
      return 0;
    } else {
      std::fprintf(stderr, "Unknown option: %s\n", arg.c_str());
      return 1;
    }
  }

  PrintConfig(cfg);

  // Parse workload list
  std::vector<std::string> workloads;
  if (cfg.benchmarks == "all") {
    workloads = {"fillseq",    "readseq",   "fillrandom",       "readrandom", "readmissing",
                 "seekrandom", "overwrite", "readwhilewriting", "fillsync"};
  } else {
    workloads = Split(cfg.benchmarks, ',');
  }

  // Determine engines to run
  std::vector<std::string> engines;
  if (cfg.engine == "both") {
    engines = {"strata", "leveldb"};
  } else if (cfg.engine == "strata" || cfg.engine == "leveldb") {
    engines = {cfg.engine};
  } else {
    engines = Split(cfg.engine, ',');
  }

  PrintTableHeader();

  std::vector<BenchmarkResult> all_results;
  for (const auto& eng : engines) {
    std::unique_ptr<BenchmarkAdapter> adapter;
    if (eng == "strata" || eng == "StrataDB") {
      adapter = CreateStrataAdapter();
    } else if (eng == "leveldb" || eng == "LevelDB") {
      adapter = CreateLevelDBAdapter();
    } else {
      std::fprintf(stderr, "Unrecognized engine: %s\n", eng.c_str());
      continue;
    }

    BenchmarkRunner runner(cfg, std::move(adapter));
    auto res = runner.RunWorkloads(workloads);
    all_results.insert(all_results.end(), res.begin(), res.end());
  }

  std::cout << "-----------------------------------------------------------------------------------"
               "--------------------------\n";

  AppendCsv(cfg.csv_path, all_results);
  std::cout << "\nResults successfully written to: " << cfg.csv_path << "\n";

  return 0;
}

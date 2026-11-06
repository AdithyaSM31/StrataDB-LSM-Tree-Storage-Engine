#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "bench/adapter.h"
#include "strata/status.h"
#include "util/histogram.h"
#include "util/zipfian.h"

#ifndef STRATA_BENCH_COMMIT
#define STRATA_BENCH_COMMIT "unknown"
#endif
#ifndef STRATA_BENCH_LEVELDB_FEATURES
#define STRATA_BENCH_LEVELDB_FEATURES "unknown"
#endif

namespace strata::bench {

namespace {

constexpr uint64_t kDefaultSeed = 133742;
constexpr size_t kKeySize = 16;
constexpr size_t kScanLength = 100;
constexpr uint64_t kMaxPrintedErrors = 5;
constexpr size_t kCacheLineSize = 64;

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

// Compaction targets. Both engines use these values and the same scoring rule,
// so "no level is over target" means the same thing for each of them.
constexpr uint64_t kL0CompactionTrigger = 4;
constexpr uint64_t kBaseLevelBytes = uint64_t{10} * 1024 * 1024;
// StrataDB reports level sizes rounded to 0.01 MB. Without this slack a level
// just under target could look over it forever, and the wait would never end.
constexpr uint64_t kLevelSizeSlack = uint64_t{16} * 1024;
constexpr auto kSettlePollInterval = std::chrono::milliseconds(100);
constexpr auto kSettleTimeout = std::chrono::minutes(30);

// Bump when the way datasets are generated changes, so old ones are rebuilt.
constexpr int kDatasetFormatVersion = 2;
constexpr std::string_view kCsvHeader =
    "run_id,commit,engine,workload,threads,num_ops,total_sec,ops_per_sec,mb_per_sec,p50_us,p99_us,"
    "p999_us,avg_us,min_us,max_us,entries,value_size,write_buffer_size,block_size,block_cache_size,"
    "bloom_bits,max_open_files,dataset,levels_at_start,writer_ops,writer_ops_per_sec,"
    "zipf_theta,read_ops,update_ops,read_p50_us,read_p99_us,read_p999_us,update_p50_us,"
    "update_p99_us,update_p999_us,group_commit,cache_hits,cache_misses";
constexpr std::string_view kTimeseriesHeader =
    "run_id,engine,workload,threads,elapsed_sec,ops_done,ops_per_sec,l0_files,level_files";

struct BenchConfig {
  std::string engine = "both";
  std::string benchmarks = "all";
  size_t num = 1000000;
  size_t sync_num = 0;  // 0 means auto: min(num, 1000)
  int threads = 1;
  std::string db_dir = "./strata_bench_data";
  size_t write_buffer_size = size_t{4} * 1024 * 1024;
  size_t block_size = 4096;
  size_t block_cache_size = size_t{8} * 1024 * 1024;
  int bloom_filter_bits = 10;
  int max_open_files = 1000;
  size_t value_size = 100;
  uint64_t write_rate = 10000;  // readwhilewriting writes per second; 0 means unthrottled
  size_t ycsb_ops = 0;          // 0 means: use num (overridden per-run if set)
  double zipf_theta = 0.99;     // Zipfian skew parameter; must be in (0, 1)
  bool leveldb_mmap = true;     // LevelDB's default; false makes it read with pread
  bool group_commit = true;     // StrataDB only; false gives every write its own sync
  std::string timeseries_path;  // empty means no time series
  uint64_t timeseries_interval_ms = 1000;
  bool stats = false;
  std::string csv_path = "bench/results/benchmarks.csv";
  std::string run_id;
};

struct BenchmarkResult {
  std::string engine;
  std::string workload;
  int threads = 1;
  uint64_t num_ops = 0;
  uint64_t errors = 0;
  double elapsed_seconds = 0.0;
  double ops_per_sec = 0.0;
  double mb_per_sec = 0.0;
  double p50_us = 0.0;
  double p99_us = 0.0;
  double p999_us = 0.0;
  double avg_us = 0.0;
  double min_us = 0.0;
  double max_us = 0.0;
  std::string dataset = "new";
  std::string levels_at_start = "-";
  uint64_t writer_ops = 0;
  double writer_ops_per_sec = 0.0;
  // YCSB workloads only: per-category percentiles.
  double zipf_theta = -1.0;  // negative means "not a YCSB workload"
  uint64_t read_ops = 0;
  uint64_t update_ops = 0;
  double read_p50_us = 0.0;
  double read_p99_us = 0.0;
  double read_p999_us = 0.0;
  double update_p50_us = 0.0;
  double update_p99_us = 0.0;
  double update_p999_us = 0.0;
  // Block cache lookups during the workload, not counting setup.
  uint64_t cache_hits = 0;
  uint64_t cache_misses = 0;
};

// One per thread, each on its own cache lines: latency recording writes to these
// fields on every operation, and threads must not slow each other down doing it.
struct alignas(kCacheLineSize) WorkerStats {
  Histogram latency;       // general operations / YCSB updates
  Histogram read_latency;  // YCSB reads
  uint64_t errors = 0;
  uint64_t bytes = 0;
  std::atomic<uint64_t> ops_done{0};  // only its worker writes it; the sampler reads it
};

std::string FormatKey(uint64_t k) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%016" PRIu64, k);
  return std::string(buf, kKeySize);
}

// A key that can never exist but still falls inside the stored key range, so
// lookups have to consult files and their bloom filters instead of being ruled
// out by the files' key ranges. Stored keys are 16 digits, so "<k>." sorts
// between k and k + 1.
//
// Don't make these keys differ from stored ones in only the last byte. Replacing
// the last digit with '.' gave a key one byte away from ten stored keys in the
// same block. LevelDB's per-block filters let 18.6% of those through, and only
// 0.90% of these, which turned a 1.5x difference between the engines into 34x.
std::string FormatMissingKey(uint64_t k, uint64_t num_entries) {
  if (k + 1 >= num_entries && k > 0) {
    --k;  // the gap after the largest stored key is outside the key range
  }
  return FormatKey(k) + ".";
}

class ValueGenerator {
 public:
  explicit ValueGenerator(size_t max_val_size) {
    const size_t sz = std::max(size_t{1024} * 1024, max_val_size * 2 + 1024);
    std::mt19937 rng(42);
    buffer_.resize(sz);
    for (char& c : buffer_) {
      c = static_cast<char>('a' + (rng() % 26));
    }
  }

  std::string_view Generate(uint64_t key, size_t size) const {
    const size_t span = buffer_.size() - size;
    return std::string_view(buffer_).substr((key * 97) % span, size);
  }

 private:
  std::string buffer_;
};

struct KeyRange {
  size_t begin;
  size_t end;
};

KeyRange PartitionFor(int thread, int threads, size_t n) {
  const size_t chunk = n / static_cast<size_t>(threads);
  const auto t = static_cast<size_t>(thread);
  return {t * chunk, (thread == threads - 1) ? n : (t + 1) * chunk};
}

// Each thread gets its own slice of [0, n). With a seed the slice is shuffled,
// and the same seed gives both engines the same sequence.
std::vector<std::vector<uint64_t>> MakeKeys(size_t n, int threads, std::optional<uint64_t> seed) {
  std::vector<std::vector<uint64_t>> keys(static_cast<size_t>(threads));
  for (int t = 0; t < threads; ++t) {
    const KeyRange range = PartitionFor(t, threads, n);
    auto& slice = keys[static_cast<size_t>(t)];
    slice.resize(range.end - range.begin);
    std::iota(slice.begin(), slice.end(), uint64_t{range.begin});
    if (seed.has_value()) {
      std::mt19937_64 rng(*seed + static_cast<uint64_t>(t));
      std::shuffle(slice.begin(), slice.end(), rng);
    }
  }
  return keys;
}

// Starts every thread at once and returns the seconds until the last one ends.
template <typename Body>
double RunWorkers(std::vector<WorkerStats>* stats, Body body) {
  const auto count = static_cast<int>(stats->size());
  std::atomic<bool> go{false};
  std::atomic<int> ready{0};
  std::vector<std::thread> workers;
  workers.reserve(stats->size());
  for (int t = 0; t < count; ++t) {
    workers.emplace_back([t, stats, &body, &go, &ready]() {
      ready.fetch_add(1);
      while (!go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      body(t, &(*stats)[static_cast<size_t>(t)]);
    });
  }
  while (ready.load() < count) {
    std::this_thread::yield();
  }
  const auto start = std::chrono::steady_clock::now();
  go.store(true, std::memory_order_release);
  for (auto& w : workers) {
    w.join();
  }
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

template <typename Op>
void TimeOp(Histogram* hist, Op op) {
  const auto t0 = std::chrono::steady_clock::now();
  op();
  const auto t1 = std::chrono::steady_clock::now();
  hist->Add(std::chrono::duration<double, std::micro>(t1 - t0).count());
}

template <typename Op>
void TimeOp(WorkerStats* worker, Op op) {
  TimeOp(&worker->latency, std::move(op));
}

bool NeedsCompaction(const std::vector<LevelShape>& shapes) {
  if (!shapes.empty() && shapes[0].files >= kL0CompactionTrigger) {
    return true;
  }
  // The deepest level has no target, so it is left out.
  uint64_t target = kBaseLevelBytes;
  for (size_t level = 1; level + 1 < shapes.size(); ++level) {
    if (shapes[level].bytes >= target + kLevelSizeSlack) {
      return true;
    }
    target *= 10;
  }
  return false;
}

std::string FormatFileCounts(const std::vector<LevelShape>& shapes) {
  std::string out;
  for (const LevelShape& shape : shapes) {
    if (!out.empty()) {
      out += '/';
    }
    out += std::to_string(shape.files);
  }
  return out;
}

void PrintConfig(const BenchConfig& cfg) {
  std::cout << "================================================================================\n"
            << "StrataDB Benchmark Harness\n"
            << "================================================================================\n"
            << "Run ID:          " << cfg.run_id << "\n"
            << "Commit:          " << STRATA_BENCH_COMMIT << "\n"
            << "Engines:         " << cfg.engine << "\n"
            << "Benchmarks:      " << cfg.benchmarks << "\n"
            << "Entries:         " << cfg.num << "\n"
            << "Value Size:      " << cfg.value_size << " bytes\n"
            << "Key Size:        " << kKeySize << " bytes\n"
            << "Threads:         " << cfg.threads << "\n"
            << "Write Buffer:    " << cfg.write_buffer_size << " bytes\n"
            << "Block Size:      " << cfg.block_size << " bytes\n"
            << "Block Cache:     " << cfg.block_cache_size << " bytes\n"
            << "Bloom Filter:    " << cfg.bloom_filter_bits << " bits/key\n"
            << "Max Open Files:  " << cfg.max_open_files << "\n"
            << "Writer Rate:     ";
  if (cfg.write_rate > 0) {
    std::cout << cfg.write_rate << " writes/s (readwhilewriting)\n";
  } else {
    std::cout << "unthrottled (readwhilewriting)\n";
  }
  std::cout << "Compression:     none on both engines\n"
            << "Checksums:       every block read verified on both engines\n"
            << "LevelDB Build:   " << STRATA_BENCH_LEVELDB_FEATURES << "\n"
            << "LevelDB Reads:   " << (cfg.leveldb_mmap ? "mmap" : "pread") << "\n"
            << "Group Commit:    " << (cfg.group_commit ? "on" : "off") << "\n"
            << "YCSB Ops:        " << (cfg.ycsb_ops > 0 ? cfg.ycsb_ops : cfg.num)
            << " (per workload)\n"
            << "Zipf Theta:      " << cfg.zipf_theta << "\n"
            << "DB Directory:    " << cfg.db_dir << "\n"
            << "CSV Output:      " << cfg.csv_path << "\n"
            << "================================================================================\n"
            << "\n";
}

const char* const kTableRule =
    "-----------------------------------------------------------------------------------"
    "---------------------------------------";

void PrintTableHeader() {
  std::cout << kTableRule << "\n";
  std::printf("%-10s %-17s %7s %12s %9s %9s %9s %10s %7s  %s\n", "Engine", "Workload", "Threads",
              "Ops/sec", "MB/s", "p50 (us)", "p99 (us)", "p99.9 (us)", "Errors", "Dataset");
  std::cout << kTableRule << "\n";
  std::cout.flush();
}

void PrintTableRow(const BenchmarkResult& r) {
  std::printf("%-10s %-17s %7d %12.1f %9.2f %9.2f %9.2f %10.2f %7" PRIu64 "  %s\n",
              r.engine.c_str(), r.workload.c_str(), r.threads, r.ops_per_sec, r.mb_per_sec,
              r.p50_us, r.p99_us, r.p999_us, r.errors, r.dataset.c_str());
  if (r.workload == "readwhilewriting") {
    std::printf("%-10s   writer: %" PRIu64 " writes at %.0f/s\n", "", r.writer_ops,
                r.writer_ops_per_sec);
  }
  if (r.zipf_theta >= 0.0) {
    // YCSB workload: show read and update latencies separately.
    if (r.update_ops > 0) {
      std::printf("%-10s   reads: %" PRIu64
                  " ops, p50=%.2f p99=%.2f p999=%.2f us | updates: %" PRIu64
                  " ops, p50=%.2f p99=%.2f p999=%.2f us\n",
                  "", r.read_ops, r.read_p50_us, r.read_p99_us, r.read_p999_us, r.update_ops,
                  r.update_p50_us, r.update_p99_us, r.update_p999_us);
    } else {
      std::printf("%-10s   reads: %" PRIu64 " ops, p50=%.2f p99=%.2f p999=%.2f us\n", "",
                  r.read_ops, r.read_p50_us, r.read_p99_us, r.read_p999_us);
    }
  }
  const uint64_t lookups = r.cache_hits + r.cache_misses;
  if (lookups > 0) {
    std::printf("%-10s   block cache: %" PRIu64 " hits, %" PRIu64 " misses, %.1f%% hit rate\n", "",
                r.cache_hits, r.cache_misses,
                100.0 * static_cast<double>(r.cache_hits) / static_cast<double>(lookups));
  }
  std::fflush(stdout);
}

// Checked before any workload runs, so a bad path can't lose a long run's results.
bool PrepareCsv(const std::string& csv_path, std::string_view header = kCsvHeader) {
  const std::filesystem::path path(csv_path);
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      std::cerr << "Cannot create " << path.parent_path().string() << ": " << ec.message() << "\n";
      return false;
    }
  }
  {
    std::ifstream existing(csv_path);
    std::string first_line;
    if (existing.is_open() && std::getline(existing, first_line) && first_line != header) {
      std::cerr << "CSV file " << csv_path
                << " has different columns. Move it aside or pass another --csv.\n";
      return false;
    }
    if (!first_line.empty()) {
      return true;
    }
  }
  std::ofstream csv(csv_path, std::ios::app);
  csv << header << "\n";
  if (!csv.good()) {
    std::cerr << "Cannot write CSV file: " << csv_path << "\n";
    return false;
  }
  return true;
}

// One side of a YCSB mix. With no operations there is no latency, and 0.00 would
// read as a very fast one, so the columns stay blank.
std::string LatencyColumns(uint64_t ops, double p50, double p99, double p999) {
  if (ops == 0) {
    return ",,";
  }
  char columns[96];
  std::snprintf(columns, sizeof(columns), "%.2f,%.2f,%.2f", p50, p99, p999);
  return columns;
}

bool AppendCsv(const BenchConfig& cfg, const BenchmarkResult& r) {
  std::ostringstream row;
  row << cfg.run_id << ',' << STRATA_BENCH_COMMIT << ',' << r.engine << ',' << r.workload << ','
      << r.threads << ',' << r.num_ops << ',';
  char numbers[256];
  std::snprintf(numbers, sizeof(numbers), "%.4f,%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,",
                r.elapsed_seconds, r.ops_per_sec, r.mb_per_sec, r.p50_us, r.p99_us, r.p999_us,
                r.avg_us, r.min_us, r.max_us);
  row << numbers << cfg.num << ',' << cfg.value_size << ',' << cfg.write_buffer_size << ','
      << cfg.block_size << ',' << cfg.block_cache_size << ',' << cfg.bloom_filter_bits << ','
      << cfg.max_open_files << ',' << r.dataset << ',' << r.levels_at_start << ',' << r.writer_ops
      << ',';
  std::snprintf(numbers, sizeof(numbers), "%.1f", r.writer_ops_per_sec);
  row << numbers << ',';
  // YCSB-specific columns: empty for non-YCSB workloads.
  if (r.zipf_theta >= 0.0) {
    std::snprintf(numbers, sizeof(numbers), "%.4f", r.zipf_theta);
    row << numbers << ',' << r.read_ops << ',' << r.update_ops << ','
        << LatencyColumns(r.read_ops, r.read_p50_us, r.read_p99_us, r.read_p999_us) << ','
        << LatencyColumns(r.update_ops, r.update_p50_us, r.update_p99_us, r.update_p999_us);
  } else {
    row << ",,,,,,,,";
  }
  row << ',' << (cfg.group_commit ? 1 : 0) << ',' << r.cache_hits << ',' << r.cache_misses << '\n';

  std::ofstream csv(cfg.csv_path, std::ios::app);
  csv << row.str();
  csv.flush();
  if (!csv.good()) {
    std::cerr << "Failed to write results to " << cfg.csv_path << "\n";
    return false;
  }
  return true;
}

// Records progress and the LSM's shape at a fixed interval while workers write.
// Rows stay in memory until the end, so sampling adds no file I/O during the run.
class TimeseriesSampler {
 public:
  TimeseriesSampler(const BenchConfig& cfg, BenchmarkAdapter* adapter, std::string engine,
                    std::string workload, const std::vector<WorkerStats>* stats)
      : cfg_(cfg),
        adapter_(adapter),
        engine_(std::move(engine)),
        workload_(std::move(workload)),
        stats_(stats),
        start_(std::chrono::steady_clock::now()),
        last_time_(start_),
        thread_([this]() { Run(); }) {}

  TimeseriesSampler(const TimeseriesSampler&) = delete;
  TimeseriesSampler& operator=(const TimeseriesSampler&) = delete;

  // Takes a last sample, so the final row shows every operation, and writes the
  // rows. Returns false if a sample or the write failed.
  bool Finish() {
    stop_.store(true, std::memory_order_relaxed);
    thread_.join();
    Sample();

    std::ofstream csv(cfg_.timeseries_path, std::ios::app);
    csv << rows_.str();
    csv.flush();
    if (!csv.good()) {
      std::cerr << "Failed to write " << cfg_.timeseries_path << "\n";
      return false;
    }
    return !failed_;
  }

 private:
  // Sleeps in short steps, so Finish() never waits long for this thread to end.
  void Run() {
    const auto interval = std::chrono::milliseconds(cfg_.timeseries_interval_ms);
    auto next_sample = start_ + interval;
    while (!stop_.load(std::memory_order_relaxed)) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_sample) {
        Sample();
        next_sample += interval;
      } else {
        std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(
            next_sample - now, std::chrono::milliseconds(10)));
      }
    }
  }

  void Sample() {
    std::vector<LevelShape> shapes;
    const Status s = adapter_->LevelShapes(&shapes);
    if (!s.ok() || shapes.empty()) {
      if (!failed_) {
        std::cerr << engine_ << ": cannot sample level sizes: " << s.ToString() << "\n";
      }
      failed_ = true;
      return;
    }
    uint64_t ops = 0;
    for (const WorkerStats& w : *stats_) {
      ops += w.ops_done.load(std::memory_order_relaxed);
    }
    const auto now = std::chrono::steady_clock::now();
    const double interval = std::chrono::duration<double>(now - last_time_).count();
    const double rate = interval > 0.0 ? static_cast<double>(ops - last_ops_) / interval : 0.0;
    char numbers[64];
    std::snprintf(numbers, sizeof(numbers), "%.3f,%" PRIu64 ",%.1f,",
                  std::chrono::duration<double>(now - start_).count(), ops, rate);
    rows_ << cfg_.run_id << ',' << engine_ << ',' << workload_ << ',' << cfg_.threads << ','
          << numbers << shapes[0].files << ',' << FormatFileCounts(shapes) << '\n';
    last_time_ = now;
    last_ops_ = ops;
  }

  const BenchConfig& cfg_;
  BenchmarkAdapter* const adapter_;
  const std::string engine_;
  const std::string workload_;
  const std::vector<WorkerStats>* const stats_;
  const std::chrono::steady_clock::time_point start_;
  std::atomic<bool> stop_{false};
  // Used by the sampling thread, then by Finish() once that thread has ended.
  std::chrono::steady_clock::time_point last_time_;
  uint64_t last_ops_ = 0;
  std::ostringstream rows_;
  bool failed_ = false;
  std::thread thread_;  // last, so everything it uses exists before it starts
};

class BenchmarkRunner {
 public:
  BenchmarkRunner(const BenchConfig& cfg, std::unique_ptr<BenchmarkAdapter> adapter)
      : cfg_(cfg),
        adapter_(std::move(adapter)),
        val_gen_(cfg.value_size),
        engine_name_(adapter_->Name()),
        engine_db_dir_(cfg_.db_dir + "/" + (engine_name_ == "StrataDB" ? "strata" : "leveldb")),
        dataset_marker_(engine_db_dir_ + "/.bench_dataset") {}

  // Returns the number of workloads that failed.
  int RunWorkloads(const std::vector<std::string>& workloads) {
    int failures = 0;
    for (const std::string& workload : workloads) {
      printed_errors_.store(0);
      std::optional<BenchmarkResult> result = RunSingleWorkload(workload);
      if (!result.has_value()) {
        // The database could not be set up, so later workloads have nothing to run on.
        std::cerr << engine_name_ << ": stopping before " << workload << " could run\n";
        ++failures;
        break;
      }
      RecordCacheStats(&*result);
      PrintTableRow(*result);
      // Numbers from a run with errors don't describe the engine, so they aren't kept.
      if (result->errors > 0) {
        std::cerr << engine_name_ << " " << workload << ": FAILED with " << result->errors
                  << " errors; not written to the CSV\n";
        ++failures;
      } else if (!AppendCsv(cfg_, *result)) {
        ++failures;
      }
      if (cfg_.stats && IsWriteWorkload(workload) && !PrintStats()) {
        ++failures;
      }
    }
    adapter_->Close();
    return failures + side_failures_;
  }

 private:
  // Counters start at zero when a database opens. Setup, such as settling
  // compactions, is left out by reading them again just before the workload.
  void MarkCacheStatsStart() {
    uint64_t hits = 0;
    uint64_t misses = 0;
    if (adapter_->BlockCacheStats(&hits, &misses).ok()) {
      cache_hits_at_start_ = hits;
      cache_misses_at_start_ = misses;
    }
  }

  void RecordCacheStats(BenchmarkResult* r) {
    uint64_t hits = 0;
    uint64_t misses = 0;
    const Status s = adapter_->BlockCacheStats(&hits, &misses);
    if (!s.ok()) {
      std::cerr << engine_name_ << ": cannot read block cache counters: " << s.ToString() << "\n";
      ++side_failures_;
      return;
    }
    r->cache_hits = hits - cache_hits_at_start_;
    r->cache_misses = misses - cache_misses_at_start_;
  }

  static bool IsWriteWorkload(const std::string& workload) {
    return workload == "fillseq" || workload == "fillrandom" || workload == "overwrite" ||
           workload == "fillsync" || workload == "readwhilewriting" || workload == "ycsba" ||
           workload == "ycsbb";
  }

  // Counters live for one open of the database, and compactions the workload caused
  // may still be running. Reports are taken here, before anything reopens it.
  bool PrintStats() {
    std::vector<LevelShape> shapes;
    if (!WaitForCompactions(&shapes)) {
      return false;
    }
    std::cout << "\n--- " << engine_name_ << " Stats ---\n" << adapter_->Stats() << "\n";
    std::cout.flush();
    return true;
  }

  BenchmarkOptions MakeOptions() const {
    BenchmarkOptions opts;
    opts.write_buffer_size = cfg_.write_buffer_size;
    opts.block_size = cfg_.block_size;
    opts.block_cache_size = cfg_.block_cache_size;
    opts.bloom_filter_bits = cfg_.bloom_filter_bits;
    opts.max_open_files = cfg_.max_open_files;
    opts.group_commit = cfg_.group_commit;
    return opts;
  }

  // Everything that changes what a dataset looks like on disk. A dataset built
  // with other values must be rebuilt, not read as though it matched. The block
  // cache and open-file limits only affect reads, so they can vary over one dataset.
  std::string DatasetFingerprint() const {
    std::ostringstream out;
    out << "format=" << kDatasetFormatVersion << " num=" << cfg_.num
        << " value_size=" << cfg_.value_size << " write_buffer=" << cfg_.write_buffer_size
        << " block_size=" << cfg_.block_size << " bloom_bits=" << cfg_.bloom_filter_bits
        << " threads=" << cfg_.threads;
    return out.str();
  }

  // Returns how the dataset was made if it exists and matches this run's options.
  std::optional<std::string> ReadDatasetLabel() const {
    std::ifstream marker(dataset_marker_);
    std::string fingerprint;
    std::string label;
    if (std::getline(marker, fingerprint) && std::getline(marker, label) &&
        fingerprint == DatasetFingerprint()) {
      return label;
    }
    return std::nullopt;
  }

  bool WriteDatasetMarker(const std::string& label) {
    std::ofstream marker(dataset_marker_, std::ios::trunc);
    marker << DatasetFingerprint() << "\n" << label << "\n";
    marker.flush();
    if (!marker.good()) {
      std::cerr << "Cannot write " << dataset_marker_ << "\n";
      return false;
    }
    return true;
  }

  // Called before a workload changes the data, so a run that dies partway leaves
  // a dataset that gets rebuilt rather than one that claims to be intact.
  bool InvalidateDataset() {
    std::error_code ec;
    std::filesystem::remove(dataset_marker_, ec);
    if (ec) {
      std::cerr << "Cannot remove " << dataset_marker_ << ": " << ec.message() << "\n";
      return false;
    }
    return true;
  }

  bool OpenFreshDatabase() {
    adapter_->Close();
    std::error_code ec;
    std::filesystem::remove_all(engine_db_dir_, ec);
    if (!ec) {
      std::filesystem::create_directories(engine_db_dir_, ec);
    }
    if (ec) {
      std::cerr << "Cannot recreate " << engine_db_dir_ << ": " << ec.message() << "\n";
      return false;
    }
    const Status s = adapter_->Open(engine_db_dir_, MakeOptions());
    if (!s.ok()) {
      std::cerr << "Failed to open fresh database at " << engine_db_dir_ << ": " << s.ToString()
                << "\n";
      return false;
    }
    return true;
  }

  bool WaitForCompactions(std::vector<LevelShape>* shapes) {
    const auto deadline = std::chrono::steady_clock::now() + kSettleTimeout;
    std::vector<LevelShape> previous;
    while (true) {
      const Status s = adapter_->LevelShapes(shapes);
      if (!s.ok()) {
        std::cerr << engine_name_ << ": cannot read level sizes: " << s.ToString() << "\n";
        return false;
      }
      if (!NeedsCompaction(*shapes) && *shapes == previous) {
        return true;
      }
      if (std::chrono::steady_clock::now() > deadline) {
        std::cerr << engine_name_ << ": compactions did not finish in time; files per level "
                  << FormatFileCounts(*shapes) << "\n";
        return false;
      }
      previous = *shapes;
      std::this_thread::sleep_for(kSettlePollInterval);
    }
  }

  // Leaves the database freshly opened, with a cold block cache and no compaction
  // running, so the workload measures one fixed LSM shape on each engine. On
  // success fills in the result fields that describe that starting point.
  bool PrepareDataset(BenchmarkResult* result) {
    std::optional<std::string> label = ReadDatasetLabel();
    if (!label.has_value()) {
      if (!OpenFreshDatabase()) {
        return false;
      }
      // Building a dataset is setup, so it never adds to a time series.
      const BenchmarkResult fill =
          RunPuts("fillrandom", MakeKeys(cfg_.num, cfg_.threads, kDefaultSeed), false, 0,
                  /*may_sample=*/false);
      if (fill.errors > 0) {
        std::cerr << engine_name_ << ": building the dataset failed with " << fill.errors
                  << " errors\n";
        return false;
      }
      label = "fillrandom";
      if (!WriteDatasetMarker(*label)) {
        return false;
      }
    }

    const Status s = adapter_->Open(engine_db_dir_, MakeOptions());
    if (!s.ok()) {
      std::cerr << "Failed to open existing database at " << engine_db_dir_ << ": " << s.ToString()
                << "\nIt was left in place. Delete it to have it rebuilt.\n";
      return false;
    }
    std::vector<LevelShape> shapes;
    if (!WaitForCompactions(&shapes)) {
      return false;
    }
    result->dataset = *label;
    result->levels_at_start = FormatFileCounts(shapes);
    return true;
  }

  std::optional<BenchmarkResult> RunSingleWorkload(const std::string& workload) {
    BenchmarkResult start;
    cache_hits_at_start_ = 0;  // right for a fill: its database has only just opened
    cache_misses_at_start_ = 0;

    if (workload == "fillseq" || workload == "fillrandom") {
      if (!OpenFreshDatabase()) {
        return std::nullopt;
      }
      const std::optional<uint64_t> seed =
          workload == "fillseq" ? std::nullopt : std::optional<uint64_t>(kDefaultSeed);
      BenchmarkResult r = RunPuts(workload, MakeKeys(cfg_.num, cfg_.threads, seed), false, 0);
      if (r.errors == 0 && !WriteDatasetMarker(workload)) {
        return std::nullopt;
      }
      return r;
    }

    if (workload == "fillsync") {
      if (!OpenFreshDatabase()) {
        return std::nullopt;
      }
      const size_t ops = cfg_.sync_num > 0 ? cfg_.sync_num : std::min(cfg_.num, size_t{1000});
      // Too few entries to read from, so this never counts as a dataset.
      return RunPuts(workload, MakeKeys(ops, cfg_.threads, std::nullopt), true, 0);
    }

    if (!PrepareDataset(&start)) {
      return std::nullopt;
    }
    MarkCacheStatsStart();

    BenchmarkResult r;
    if (workload == "overwrite") {
      if (!InvalidateDataset()) {
        return std::nullopt;
      }
      r = RunPuts(workload, MakeKeys(cfg_.num, cfg_.threads, kDefaultSeed + 1000), false, 9999);
      if (r.errors == 0 && !WriteDatasetMarker(start.dataset + "+overwrite")) {
        return std::nullopt;
      }
    } else if (workload == "readseq") {
      r = RunReadSeq();
    } else if (workload == "readrandom") {
      r = RunReadRandom();
    } else if (workload == "readmissing") {
      r = RunReadMissing();
    } else if (workload == "seekrandom") {
      r = RunSeekRandom();
    } else if (workload == "scan") {
      r = RunScan();
    } else if (workload == "readwhilewriting") {
      if (!InvalidateDataset()) {
        return std::nullopt;
      }
      r = RunReadWhileWriting();
    } else if (workload == "ycsbc") {
      r = RunYcsb(workload, /*read_frac=*/1.0);
    } else if (workload == "ycsbb") {
      if (!InvalidateDataset()) {
        return std::nullopt;
      }
      r = RunYcsb(workload, /*read_frac=*/0.95);
      if (r.errors == 0 && !WriteDatasetMarker(start.dataset + "+ycsbb")) {
        return std::nullopt;
      }
    } else if (workload == "ycsba") {
      if (!InvalidateDataset()) {
        return std::nullopt;
      }
      r = RunYcsb(workload, /*read_frac=*/0.50);
      if (r.errors == 0 && !WriteDatasetMarker(start.dataset + "+ycsba")) {
        return std::nullopt;
      }
    }
    r.dataset = start.dataset;
    r.levels_at_start = start.levels_at_start;
    return r;
  }

  void CountError(WorkerStats* worker, const char* op, std::string_view key,
                  const std::string& detail) {
    ++worker->errors;
    if (printed_errors_.fetch_add(1, std::memory_order_relaxed) < kMaxPrintedErrors) {
      std::cerr << engine_name_ + " " + op + " error, key " + std::string(key) + ": " + detail +
                       "\n";
    }
  }

  BenchmarkResult RunPuts(const std::string& workload,
                          const std::vector<std::vector<uint64_t>>& keys, bool sync,
                          uint64_t value_salt, bool may_sample = true) {
    std::vector<WorkerStats> stats(keys.size());
    uint64_t ops = 0;
    for (const auto& slice : keys) {
      ops += slice.size();
    }
    std::optional<TimeseriesSampler> sampler;
    if (may_sample && !cfg_.timeseries_path.empty()) {
      sampler.emplace(cfg_, adapter_.get(), engine_name_, workload, &stats);
    }
    const double elapsed = RunWorkers(&stats, [&](int t, WorkerStats* w) {
      for (uint64_t k : keys[static_cast<size_t>(t)]) {
        const std::string key = FormatKey(k);
        const std::string_view value = val_gen_.Generate(k + value_salt, cfg_.value_size);
        Status s;
        TimeOp(w, [&]() { s = sync ? adapter_->SyncPut(key, value) : adapter_->Put(key, value); });
        if (s.ok()) {
          w->bytes += key.size() + value.size();
        } else {
          CountError(w, "put", key, s.ToString());
        }
        w->ops_done.store(w->ops_done.load(std::memory_order_relaxed) + 1,
                          std::memory_order_relaxed);
      }
    });
    if (sampler.has_value() && !sampler->Finish()) {
      ++side_failures_;
    }
    return BuildResult(workload, ops, elapsed, stats);
  }

  // Reads by iterating, which is what this workload is for: it is the one that
  // measures the merging iterator walking the whole key space.
  BenchmarkResult RunReadSeq() {
    const size_t n = cfg_.num;
    std::vector<WorkerStats> stats(static_cast<size_t>(cfg_.threads));
    const double elapsed = RunWorkers(&stats, [&](int t, WorkerStats* w) {
      const KeyRange range = PartitionFor(t, cfg_.threads, n);
      const std::string first_key = FormatKey(range.begin);
      std::unique_ptr<BenchmarkCursor> cursor = adapter_->NewCursor();
      if (cursor == nullptr) {
        CountError(w, "readseq", first_key, "no cursor");
        return;
      }
      cursor->Seek(first_key);
      if (range.begin < range.end && (!cursor->Valid() || cursor->key() != first_key)) {
        CountError(w, "readseq", first_key, "first key of the range was not found");
      }
      size_t seen = 0;
      while (seen < range.end - range.begin && cursor->Valid()) {
        w->bytes += cursor->key().size() + cursor->value().size();
        ++seen;
        TimeOp(w, [&]() { cursor->Next(); });
      }
      const Status s = cursor->status();
      if (!s.ok()) {
        CountError(w, "readseq", first_key, s.ToString());
      }
      if (seen != range.end - range.begin) {
        CountError(w, "readseq", first_key,
                   "saw " + std::to_string(seen) + " entries of " +
                       std::to_string(range.end - range.begin));
      }
    });
    return BuildResult("readseq", n, elapsed, stats);
  }

  void GetExistingKeys(uint64_t seed, size_t ops, WorkerStats* w) {
    std::mt19937_64 rng(seed);
    std::string value;
    for (size_t i = 0; i < ops; ++i) {
      const std::string key = FormatKey(rng() % cfg_.num);
      Status s;
      TimeOp(w, [&]() { s = adapter_->Get(key, &value); });
      if (!s.ok()) {
        CountError(w, "get", key, s.ToString());
      } else if (value.size() != cfg_.value_size) {
        CountError(w, "get", key, "value has " + std::to_string(value.size()) + " bytes");
      } else {
        w->bytes += key.size() + value.size();
      }
    }
  }

  BenchmarkResult RunReadRandom() {
    const size_t n = cfg_.num;
    std::vector<WorkerStats> stats(static_cast<size_t>(cfg_.threads));
    const double elapsed = RunWorkers(&stats, [&](int t, WorkerStats* w) {
      const KeyRange range = PartitionFor(t, cfg_.threads, n);
      GetExistingKeys(kDefaultSeed + 2000 + static_cast<uint64_t>(t), range.end - range.begin, w);
    });
    return BuildResult("readrandom", n, elapsed, stats);
  }

  BenchmarkResult RunReadMissing() {
    const size_t n = cfg_.num;
    std::vector<WorkerStats> stats(static_cast<size_t>(cfg_.threads));
    const double elapsed = RunWorkers(&stats, [&](int t, WorkerStats* w) {
      const KeyRange range = PartitionFor(t, cfg_.threads, n);
      std::mt19937_64 rng(kDefaultSeed + 3000 + static_cast<uint64_t>(t));
      std::string value;
      for (size_t i = range.begin; i < range.end; ++i) {
        const std::string key = FormatMissingKey(rng() % n, n);
        Status s;
        TimeOp(w, [&]() { s = adapter_->Get(key, &value); });
        if (s.ok()) {
          CountError(w, "readmissing", key, "found a key that should not exist");
        } else if (!s.IsNotFound()) {
          CountError(w, "readmissing", key, s.ToString());
        } else {
          w->bytes += key.size();
        }
      }
    });
    return BuildResult("readmissing", n, elapsed, stats);
  }

  BenchmarkResult RunSeekRandom() {
    const size_t n = cfg_.num;
    std::vector<WorkerStats> stats(static_cast<size_t>(cfg_.threads));
    const double elapsed = RunWorkers(&stats, [&](int t, WorkerStats* w) {
      const KeyRange range = PartitionFor(t, cfg_.threads, n);
      std::mt19937_64 rng(kDefaultSeed + 4000 + static_cast<uint64_t>(t));
      for (size_t i = range.begin; i < range.end; ++i) {
        const std::string key = FormatKey(rng() % n);
        std::unique_ptr<BenchmarkCursor> cursor;
        TimeOp(w, [&]() {
          cursor = adapter_->NewCursor();
          if (cursor != nullptr) {
            cursor->Seek(key);
          }
        });
        if (cursor == nullptr) {
          CountError(w, "seekrandom", key, "no cursor");
          continue;
        }
        const Status s = cursor->status();
        if (!s.ok()) {
          CountError(w, "seekrandom", key, s.ToString());
        } else if (!cursor->Valid() || cursor->key() != key) {
          CountError(w, "seekrandom", key, "seek did not land on the key");
        } else {
          w->bytes += cursor->key().size() + cursor->value().size();
        }
      }
    });
    return BuildResult("seekrandom", n, elapsed, stats);
  }

  // Each operation reads kScanLength entries, so it runs a tenth as many operations
  // as the other read workloads.
  BenchmarkResult RunScan() {
    const size_t n = cfg_.num;
    const size_t ops = std::max(n / 10, size_t{1});
    const size_t scan_length = std::min(kScanLength, n);
    std::vector<WorkerStats> stats(static_cast<size_t>(cfg_.threads));
    const double elapsed = RunWorkers(&stats, [&](int t, WorkerStats* w) {
      const KeyRange range = PartitionFor(t, cfg_.threads, ops);
      std::mt19937_64 rng(kDefaultSeed + 6000 + static_cast<uint64_t>(t));
      for (size_t i = range.begin; i < range.end; ++i) {
        const std::string key = FormatKey(rng() % (n - scan_length + 1));
        std::unique_ptr<BenchmarkCursor> cursor;
        size_t seen = 0;
        size_t bytes = 0;
        TimeOp(w, [&]() {
          cursor = adapter_->NewCursor();
          if (cursor == nullptr) {
            return;
          }
          for (cursor->Seek(key); seen < scan_length && cursor->Valid(); cursor->Next()) {
            bytes += cursor->key().size() + cursor->value().size();
            ++seen;
          }
        });
        if (cursor == nullptr) {
          CountError(w, "scan", key, "no cursor");
          continue;
        }
        const Status s = cursor->status();
        if (!s.ok()) {
          CountError(w, "scan", key, s.ToString());
        } else if (seen != scan_length) {
          CountError(w, "scan", key, "saw " + std::to_string(seen) + " entries");
        } else {
          w->bytes += bytes;
        }
      }
    });
    return BuildResult("scan", ops, elapsed, stats);
  }

  // Readers run against a writer at a fixed rate, which both engines see equally.
  // Left unthrottled, the faster writer would be penalised with a bigger LSM, and
  // slower readers would keep the writer going for longer, making them slower still.
  BenchmarkResult RunReadWhileWriting() {
    using Clock = std::chrono::steady_clock;
    const size_t n = cfg_.num;
    std::vector<WorkerStats> stats(static_cast<size_t>(cfg_.threads));
    WorkerStats writer_stats;
    std::atomic<bool> writer_go{false};
    std::atomic<bool> writer_stop{false};
    uint64_t writer_ops = 0;
    double writer_seconds = 0.0;

    std::thread writer([&]() {
      while (!writer_go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      const auto start = Clock::now();
      const std::chrono::duration<double> interval(
          cfg_.write_rate > 0 ? 1.0 / static_cast<double>(cfg_.write_rate) : 0.0);
      uint64_t k = n;  // above every key the readers look for
      while (!writer_stop.load(std::memory_order_relaxed)) {
        if (cfg_.write_rate > 0) {
          const auto due = start + std::chrono::duration_cast<Clock::duration>(
                                       interval * static_cast<double>(writer_ops));
          const auto now = Clock::now();
          if (now < due) {
            std::this_thread::sleep_until(std::min(due, now + std::chrono::milliseconds(1)));
            continue;
          }
        }
        const std::string key = FormatKey(k);
        const Status s = adapter_->Put(key, val_gen_.Generate(k, cfg_.value_size));
        if (!s.ok()) {
          CountError(&writer_stats, "write", key, s.ToString());
        }
        ++k;
        ++writer_ops;
      }
      writer_seconds = std::chrono::duration<double>(Clock::now() - start).count();
    });

    writer_go.store(true, std::memory_order_release);
    const double elapsed = RunWorkers(&stats, [&](int t, WorkerStats* w) {
      const KeyRange range = PartitionFor(t, cfg_.threads, n);
      GetExistingKeys(kDefaultSeed + 5000 + static_cast<uint64_t>(t), range.end - range.begin, w);
    });
    writer_stop.store(true, std::memory_order_relaxed);
    writer.join();

    BenchmarkResult r = BuildResult("readwhilewriting", n, elapsed, stats);
    r.errors += writer_stats.errors;
    r.writer_ops = writer_ops;
    r.writer_ops_per_sec =
        writer_seconds > 0.0 ? static_cast<double>(writer_ops) / writer_seconds : 0.0;
    if (cfg_.write_rate > 0 && r.writer_ops_per_sec < 0.95 * static_cast<double>(cfg_.write_rate)) {
      std::cerr << engine_name_ << ": writer only reached " << r.writer_ops_per_sec << " of "
                << cfg_.write_rate << " writes/s; compare this row with care\n";
    }
    return r;
  }

  // Runs a YCSB-style workload.  read_frac is the fraction of operations that
  // are Gets (1.0 for C, 0.95 for B, 0.50 for A); the rest are blind Puts.
  // Keys are drawn from a Zipfian distribution over [0, num).
  // Reads are recorded in read_latency; updates in latency.
  BenchmarkResult RunYcsb(const std::string& workload, double read_frac) {
    const size_t n = cfg_.num;
    const size_t ops = cfg_.ycsb_ops > 0 ? cfg_.ycsb_ops : n;
    const double theta = cfg_.zipf_theta;

    // Compute zeta(n) once, outside the timed section. O(n).
    const double zeta_n = ZipfZeta(static_cast<uint64_t>(n), theta);

    // Seed offsets chosen to differ from all other workloads.
    constexpr uint64_t kYcsbDecisionOffset = 10000;
    constexpr uint64_t kYcsbZipfOffset = 11000;
    uint64_t workload_offset = 0;
    if (workload == "ycsbb") {
      workload_offset = 100;
    } else if (workload == "ycsba") {
      workload_offset = 200;
    }

    std::vector<WorkerStats> stats(static_cast<size_t>(cfg_.threads));
    const double elapsed = RunWorkers(&stats, [&](int t, WorkerStats* w) {
      const size_t total_threads = static_cast<size_t>(cfg_.threads);
      const size_t thread_ops =
          ops / total_threads + (static_cast<size_t>(t) < ops % total_threads ? 1 : 0);

      // Per-thread Zipfian generator seeded uniquely.
      ZipfianGenerator zipf(
          static_cast<uint64_t>(n), theta, zeta_n,
          kDefaultSeed + kYcsbZipfOffset + workload_offset + static_cast<uint64_t>(t));
      // Per-thread PRNG for the read/update decision.
      std::mt19937_64 decision_rng(kDefaultSeed + kYcsbDecisionOffset + workload_offset +
                                   static_cast<uint64_t>(t));
      std::uniform_real_distribution<double> unit_dist(0.0, 1.0);

      std::string value;
      for (size_t i = 0; i < thread_ops; ++i) {
        // Choose key, operation, and value before measuring time.
        const uint64_t key_id = zipf.Next();
        const std::string key = FormatKey(key_id);
        const bool is_read = (unit_dist(decision_rng) < read_frac);

        if (is_read) {
          Status s;
          TimeOp(&w->read_latency, [&]() { s = adapter_->Get(key, &value); });
          if (!s.ok()) {
            CountError(w, "get", key, s.ToString());
          } else if (value.size() != cfg_.value_size) {
            CountError(w, "get", key, "value has " + std::to_string(value.size()) + " bytes");
          } else {
            w->bytes += key.size() + value.size();
          }
        } else {
          constexpr uint64_t kYcsbUpdateValueSalt = 10000;
          const std::string_view val =
              val_gen_.Generate(key_id + kYcsbUpdateValueSalt, cfg_.value_size);
          Status s;
          TimeOp(&w->latency, [&]() { s = adapter_->Put(key, val); });
          if (s.ok()) {
            w->bytes += key.size() + val.size();
          } else {
            CountError(w, "put", key, s.ToString());
          }
        }
      }
    });

    // Merge per-thread histograms.
    Histogram read_hist;
    Histogram update_hist;
    Histogram combined;
    uint64_t bytes = 0;
    uint64_t errors = 0;
    for (const WorkerStats& ws : stats) {
      read_hist.Merge(ws.read_latency);
      update_hist.Merge(ws.latency);
      combined.Merge(ws.read_latency);
      combined.Merge(ws.latency);
      bytes += ws.bytes;
      errors += ws.errors;
    }

    BenchmarkResult r;
    r.engine = engine_name_;
    r.workload = workload;
    r.threads = cfg_.threads;
    r.num_ops = ops;
    r.errors = errors;
    r.elapsed_seconds = elapsed;
    r.ops_per_sec = (elapsed > 0.0) ? (static_cast<double>(ops) / elapsed) : 0.0;
    r.mb_per_sec = (elapsed > 0.0) ? (static_cast<double>(bytes) / (elapsed * 1048576.0)) : 0.0;
    r.p50_us = combined.Percentile(50.0);
    r.p99_us = combined.Percentile(99.0);
    r.p999_us = combined.Percentile(99.9);
    r.avg_us = combined.Average();
    r.min_us = combined.Min();
    r.max_us = combined.Max();
    r.zipf_theta = theta;
    r.read_ops = static_cast<uint64_t>(read_hist.num());
    r.update_ops = static_cast<uint64_t>(update_hist.num());
    r.read_p50_us = read_hist.Percentile(50.0);
    r.read_p99_us = read_hist.Percentile(99.0);
    r.read_p999_us = read_hist.Percentile(99.9);
    r.update_p50_us = update_hist.Percentile(50.0);
    r.update_p99_us = update_hist.Percentile(99.0);
    r.update_p999_us = update_hist.Percentile(99.9);
    return r;
  }

  BenchmarkResult BuildResult(const std::string& workload, uint64_t ops, double elapsed,
                              const std::vector<WorkerStats>& stats) const {
    Histogram merged;
    uint64_t bytes = 0;
    uint64_t errors = 0;
    for (const WorkerStats& w : stats) {
      merged.Merge(w.latency);
      bytes += w.bytes;
      errors += w.errors;
    }

    BenchmarkResult r;
    r.engine = engine_name_;
    r.workload = workload;
    r.threads = cfg_.threads;
    r.num_ops = ops;
    r.errors = errors;
    r.elapsed_seconds = elapsed;
    r.ops_per_sec = (elapsed > 0.0) ? (static_cast<double>(ops) / elapsed) : 0.0;
    r.mb_per_sec = (elapsed > 0.0) ? (static_cast<double>(bytes) / (elapsed * 1048576.0)) : 0.0;
    r.p50_us = merged.Percentile(50.0);
    r.p99_us = merged.Percentile(99.0);
    r.p999_us = merged.Percentile(99.9);
    r.avg_us = merged.Average();
    r.min_us = merged.Min();
    r.max_us = merged.Max();
    return r;
  }

  const BenchConfig& cfg_;
  std::unique_ptr<BenchmarkAdapter> adapter_;
  ValueGenerator val_gen_;
  std::string engine_name_;
  std::string engine_db_dir_;
  std::string dataset_marker_;
  std::atomic<uint64_t> printed_errors_{0};
  uint64_t cache_hits_at_start_ = 0;
  uint64_t cache_misses_at_start_ = 0;
  int side_failures_ = 0;  // a time series or cache counter that couldn't be recorded
};

// Parses all of `text`: digits, then an optional k/m/g suffix worth `scale` to that
// power. std::stoull alone would read "1M" as 1 and "1T" as 1. With scale 1024 a
// trailing 'b' is accepted too, as in "4MB".
bool ParseUnsigned(std::string_view text, uint64_t scale, uint64_t* out) {
  uint64_t value = 0;
  const char* const end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(text.data(), end, value);
  if (ec != std::errc{}) {
    return false;
  }
  std::string_view suffix(ptr, static_cast<size_t>(end - ptr));
  const auto lower = [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  };

  uint64_t multiplier = 1;
  if (!suffix.empty()) {
    const char unit = lower(suffix.front());
    if (unit == 'k') {
      multiplier = scale;
    } else if (unit == 'm') {
      multiplier = scale * scale;
    } else if (unit == 'g') {
      multiplier = scale * scale * scale;
    }
    if (multiplier != 1) {
      suffix.remove_prefix(1);
    }
  }
  if (!suffix.empty() && !(scale == 1024 && suffix.size() == 1 && lower(suffix.front()) == 'b')) {
    return false;
  }
  if (value > std::numeric_limits<uint64_t>::max() / multiplier) {
    return false;
  }
  *out = value * multiplier;
  return true;
}

std::vector<std::string> Split(std::string_view str, char delim) {
  std::vector<std::string> result;
  std::stringstream ss{std::string(str)};
  std::string item;
  while (std::getline(ss, item, delim)) {
    if (!item.empty()) {
      result.push_back(item);
    }
  }
  return result;
}

std::string MakeRunId() {
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
  gmtime_r(&now, &utc);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &utc);
  return buf;
}

void PrintUsage() {
  std::cout
      << "Usage: strata_bench [options]\n"
      << "  --engine=strata,leveldb,both     Target database engine (default: both)\n"
      << "  --benchmarks=fillseq,readseq,... Workload list or 'all' (default: all)\n"
      << "  --num=N                          Key/value pairs, e.g. 1000000 or 1M (default: 1M)\n"
      << "  --sync_num=N                     Writes for fillsync (default: min(num, 1000))\n"
      << "  --threads=N                      Worker threads, 1-64 (default: 1)\n"
      << "  --db=PATH                        Database directory (default: ./strata_bench_data)\n"
      << "  --write_buffer_size=N            Bytes, or with KB/MB/GB (default: 4MB)\n"
      << "  --cache_size=N                   Block cache, 0 for none (default: 8MB)\n"
      << "  --block_size=N                   Block size in bytes (default: 4096)\n"
      << "  --bloom_bits=N                   Bloom bits per key, 0 for none (default: 10)\n"
      << "  --max_open_files=N               Max open files, at least 74 (default: 1000)\n"
      << "  --value_size=N                   Value size in bytes (default: 100)\n"
      << "  --write_rate=N                   readwhilewriting writes/s, 0 for no limit "
         "(default: 10000)\n"
      << "  --stats                          Print engine stats after each write workload\n"
      << "  --csv=PATH                       CSV file to append to "
         "(default: bench/results/benchmarks.csv)\n"
      << "  --ycsb_ops=N                     Operations for YCSB workloads (default: num)\n"
      << "  --zipf_theta=F                   Zipfian skew in (0,1); 0.99 is YCSB default\n"
      << "  --leveldb_mmap=0|1               0 makes LevelDB read with pread, and reports it as\n"
      << "                                   LevelDB-nommap (default: 1)\n"
      << "  --group_commit=0|1               0 gives every StrataDB write its own WAL record and\n"
      << "                                   sync; needs --engine=strata (default: 1)\n"
      << "  --timeseries=PATH                CSV of progress and files per level during fillseq,\n"
      << "                                   fillrandom, overwrite and fillsync\n"
      << "  --timeseries_interval_ms=N       Time between samples (default: 1000)\n"
      << "\n"
      << "Workloads: fillseq fillrandom overwrite fillsync readseq readrandom readmissing\n"
      << "           seekrandom scan ycsbc ycsbb ycsba readwhilewriting\n"
      << "\n"
      << "Exit status: 0 on success, 1 if any operation or workload failed, 2 for bad usage.\n";
}

// Returns an exit status if the program should stop, or nullopt to carry on.
std::optional<int> ParseArgs(int argc, char* argv[], BenchConfig* cfg) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];

    const auto value_of = [&](std::string_view flag, std::string_view* value) {
      if (arg.substr(0, flag.size()) != flag) {
        return false;
      }
      *value = arg.substr(flag.size());
      return true;
    };
    const auto parse_number = [&](std::string_view text, uint64_t scale, uint64_t min, uint64_t max,
                                  auto* field) {
      uint64_t parsed = 0;
      if (!ParseUnsigned(text, scale, &parsed) || parsed < min || parsed > max) {
        std::cerr << "Invalid value: " << arg << " (allowed range " << min << " to " << max
                  << ")\n";
        return false;
      }
      *field = static_cast<std::remove_pointer_t<decltype(field)>>(parsed);
      return true;
    };

    constexpr uint64_t kCount = 1000;
    constexpr uint64_t kBytes = 1024;
    constexpr uint64_t kNoLimit = uint64_t{1} << 40;
    constexpr uint64_t kMaxValueSize = uint64_t{1} << 30;
    // FormatKey has 16 digits, and readmissing and the writer go above num.
    constexpr uint64_t kMaxEntries = uint64_t{1000000000000000};

    std::string_view value;
    bool ok = true;
    if (value_of("--engine=", &value)) {
      cfg->engine = value;
    } else if (value_of("--benchmarks=", &value)) {
      cfg->benchmarks = value;
    } else if (value_of("--num=", &value)) {
      ok = parse_number(value, kCount, 1, kMaxEntries, &cfg->num);
    } else if (value_of("--sync_num=", &value)) {
      ok = parse_number(value, kCount, 1, kMaxEntries, &cfg->sync_num);
    } else if (value_of("--threads=", &value)) {
      ok = parse_number(value, kCount, 1, 64, &cfg->threads);
    } else if (value_of("--db=", &value)) {
      cfg->db_dir = value;
    } else if (value_of("--write_buffer_size=", &value)) {
      ok = parse_number(value, kBytes, 1, kNoLimit, &cfg->write_buffer_size);
    } else if (value_of("--cache_size=", &value)) {
      ok = parse_number(value, kBytes, 0, kNoLimit, &cfg->block_cache_size);
    } else if (value_of("--block_size=", &value)) {
      ok = parse_number(value, kBytes, 1, kNoLimit, &cfg->block_size);
    } else if (value_of("--bloom_bits=", &value)) {
      ok = parse_number(value, kCount, 0, 64, &cfg->bloom_filter_bits);
    } else if (value_of("--max_open_files=", &value)) {
      // Below 74 LevelDB raises the limit and StrataDB does not, so they'd differ.
      ok = parse_number(value, kCount, 74, 50000, &cfg->max_open_files);
    } else if (value_of("--value_size=", &value)) {
      ok = parse_number(value, kBytes, 1, kMaxValueSize, &cfg->value_size);
    } else if (value_of("--write_rate=", &value)) {
      ok = parse_number(value, kCount, 0, kNoLimit, &cfg->write_rate);
    } else if (value_of("--ycsb_ops=", &value)) {
      ok = parse_number(value, kCount, 1, kMaxEntries, &cfg->ycsb_ops);
    } else if (value_of("--zipf_theta=", &value)) {
      double parsed_theta = 0.0;
      size_t pos = 0;
      const std::string val_str(value);
      try {
        parsed_theta = std::stod(val_str, &pos);
        if (pos != val_str.size()) {
          ok = false;
        }
      } catch (...) {
        ok = false;
      }
      if (!ok || !IsValidZipfTheta(parsed_theta)) {
        std::cerr << "Invalid value: " << arg << " (zipf_theta must be in (0, 1))\n";
        ok = false;
      } else {
        cfg->zipf_theta = parsed_theta;
      }
    } else if (value_of("--leveldb_mmap=", &value)) {
      if (value != "0" && value != "1") {
        std::cerr << "Invalid value: " << arg << " (must be 0 or 1)\n";
        ok = false;
      } else {
        cfg->leveldb_mmap = value == "1";
      }
    } else if (value_of("--group_commit=", &value)) {
      if (value != "0" && value != "1") {
        std::cerr << "Invalid value: " << arg << " (must be 0 or 1)\n";
        ok = false;
      } else {
        cfg->group_commit = value == "1";
      }
    } else if (value_of("--timeseries=", &value)) {
      cfg->timeseries_path = value;
    } else if (value_of("--timeseries_interval_ms=", &value)) {
      ok = parse_number(value, kCount, 1, 3600000, &cfg->timeseries_interval_ms);
    } else if (value_of("--csv=", &value)) {
      cfg->csv_path = value;
    } else if (arg == "--stats" || arg == "--stats=1" || arg == "--stats=true") {
      cfg->stats = true;
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return kExitOk;
    } else {
      std::cerr << "Unknown option: " << arg << "\n";
      return kExitUsage;
    }
    if (!ok) {
      return kExitUsage;
    }
  }
  return std::nullopt;
}

int RunMain(int argc, char* argv[]) {
  BenchConfig cfg;
  if (const std::optional<int> exit_status = ParseArgs(argc, argv, &cfg)) {
    return *exit_status;
  }
  cfg.run_id = MakeRunId();

  const std::vector<std::string> known_workloads = {
      "fillseq", "readseq", "fillrandom", "readrandom", "readmissing",      "seekrandom", "scan",
      "ycsbc",   "ycsbb",   "ycsba",      "overwrite",  "readwhilewriting", "fillsync"};
  const std::vector<std::string> workloads =
      cfg.benchmarks == "all" ? known_workloads : Split(cfg.benchmarks, ',');
  if (workloads.empty()) {
    std::cerr << "No workloads given\n";
    return kExitUsage;
  }
  for (const std::string& workload : workloads) {
    if (std::find(known_workloads.begin(), known_workloads.end(), workload) ==
        known_workloads.end()) {
      std::cerr << "Unknown workload: " << workload << "\n";
      return kExitUsage;
    }
  }

  const std::vector<std::string> engines =
      cfg.engine == "both" ? std::vector<std::string>{"strata", "leveldb"} : Split(cfg.engine, ',');
  if (engines.empty()) {
    std::cerr << "No engines given\n";
    return kExitUsage;
  }
  for (const std::string& engine : engines) {
    if (engine != "strata" && engine != "leveldb") {
      std::cerr << "Unknown engine: " << engine << "\n";
      return kExitUsage;
    }
  }

  if (!cfg.group_commit && engines != std::vector<std::string>{"strata"}) {
    std::cerr << "--group_commit=0 needs --engine=strata: LevelDB has no way to turn it off\n";
    return kExitUsage;
  }
  if (!cfg.timeseries_path.empty()) {
    const auto samples = [](const std::string& workload) {
      return workload == "fillseq" || workload == "fillrandom" || workload == "overwrite" ||
             workload == "fillsync";
    };
    if (std::none_of(workloads.begin(), workloads.end(), samples)) {
      std::cerr << "--timeseries needs one of fillseq, fillrandom, overwrite or fillsync\n";
      return kExitUsage;
    }
  }

  if (!PrepareCsv(cfg.csv_path)) {
    return kExitFailure;
  }
  if (!cfg.timeseries_path.empty() && !PrepareCsv(cfg.timeseries_path, kTimeseriesHeader)) {
    return kExitFailure;
  }

  PrintConfig(cfg);
  PrintTableHeader();

  int failures = 0;
  for (const std::string& engine : engines) {
    BenchmarkRunner runner(
        cfg, engine == "strata" ? CreateStrataAdapter() : CreateLevelDBAdapter(cfg.leveldb_mmap));
    failures += runner.RunWorkloads(workloads);
  }

  std::cout << kTableRule << "\n";
  if (failures > 0) {
    std::cerr << "\n" << failures << " workload(s) FAILED\n";
    return kExitFailure;
  }
  std::cout << "\nResults written to: " << cfg.csv_path << "\n";
  return kExitOk;
}

}  // namespace

}  // namespace strata::bench

int main(int argc, char* argv[]) { return strata::bench::RunMain(argc, argv); }

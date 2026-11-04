#include <gtest/gtest.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <set>
#include <string>

#include "crash_model.h"
#include "strata/db.h"
#include "strata/env.h"
#include "strata/options.h"
#include "strata/status.h"

namespace strata {
namespace {

// What the child reports over the pipe after each synced write.
struct Progress {
  uint64_t durable;      // this write, and all before it, are confirmed durable
  uint64_t flushes;      // finished by this child so far
  uint64_t compactions;  // finished by this child so far
};

bool ReadExact(int fd, void* buf, size_t n) {
  char* ptr = static_cast<char*>(buf);
  while (n > 0) {
    ssize_t r = read(fd, ptr, n);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (r == 0) {
      return false;
    }
    ptr += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

uint64_t Stat(const std::string& stats, const std::string& label) {
  const auto pos = stats.find(label);
  return pos == std::string::npos ? 0 : std::stoull(stats.substr(pos + label.size()));
}

constexpr int kDrainLevelZeroTo = 8;

int LevelZeroFiles(DB* db) {
  std::string files;
  return db->GetProperty("strata.num-files-at-level0", &files) ? std::stoi(files) : 0;
}

std::set<std::string> TableFiles(const std::string& dir) {
  std::set<std::string> tables;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().extension() == ".sst") {
      tables.insert(entry.path().filename().string());
    }
  }
  return tables;
}

class KillTest : public ::testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<uint64_t> counter{0};
    test_dir_ =
        (std::filesystem::temp_directory_path() /
         ("stratadb_kill_test_" + std::to_string(::getpid()) + "_" +
          std::to_string(Env::Default()->NowMicros()) + "_" + std::to_string(counter.fetch_add(1))))
            .string();
    std::error_code ec;
    std::filesystem::create_directories(test_dir_, ec);
    ASSERT_FALSE(ec) << ec.message();
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(test_dir_, ec);
  }

  std::string test_dir_;
};

// Writes forever, from `seq` on, reporting each synced write. Never returns.
[[noreturn]] void RunChild(const Options& opts, const std::string& dir,
                           const CrashWorkload& workload, uint64_t seq, int report_fd) {
  std::unique_ptr<DB> db;
  if (!DB::Open(opts, dir, &db).ok()) {
    _exit(1);
  }
  for (;; ++seq) {
    const bool sync = seq % 8 == 0;
    if (!workload.Apply(db.get(), seq, sync).ok()) {
      _exit(2);
    }
    if (sync) {
      std::string stats;
      db->GetProperty("strata.stats", &stats);
      const Progress progress{seq, Stat(stats, "Flush count: "), Stat(stats, "Compaction count: ")};
      if (write(report_fd, &progress, sizeof(progress)) != sizeof(progress)) {
        _exit(3);
      }
    }
  }
}

TEST_F(KillTest, ThousandKillIterationsSurviveWithoutDataLoss) {
  const char* env_iter = std::getenv("STRATA_KILL_ITERATIONS");  // NOLINT(concurrency-mt-unsafe)
  const int kTotalIterations =
      (env_iter != nullptr && std::atoi(env_iter) > 0) ? std::atoi(env_iter) : 1000;

  Options opts;
  opts.create_if_missing = true;
  opts.write_buffer_size = size_t{64} * 1024;

  // Large values against a small buffer keep the child flushing, and few keys
  // make the files overlap, so its compactions merge instead of moving files.
  CrashWorkload workload;
  workload.key_space = 200;
  workload.value_size = 4096;
  CrashModel model(workload);

  uint64_t child_flushes = 0;
  uint64_t child_compactions = 0;
  int kills_leaving_orphan_tables = 0;
  std::mt19937 rnd(301);

  for (int iter = 0; iter < kTotalIterations; ++iter) {
    int pipe_fds[2];
    ASSERT_EQ(pipe(pipe_fds), 0);

    const pid_t pid = fork();
    ASSERT_GE(pid, 0) << "fork failed at iteration " << iter;

    if (pid == 0) {
      close(pipe_fds[0]);
      RunChild(opts, test_dir_, workload, model.applied() + 1, pipe_fds[1]);
    }

    close(pipe_fds[1]);

    // Let the child reach at least one synced write, then kill it at a random moment.
    uint64_t durable = 0;
    Progress progress{};
    if (ReadExact(pipe_fds[0], &progress, sizeof(progress))) {
      durable = progress.durable;
    }
    // Mostly short lives, and now and then long enough to finish background work.
    usleep(iter % 10 == 0 ? 200000 : rnd() % 30000);

    ASSERT_EQ(kill(pid, SIGKILL), 0);

    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFSIGNALED(status))
        << "child exited by itself at iteration " << iter << " with code " << WEXITSTATUS(status);
    EXPECT_EQ(WTERMSIG(status), SIGKILL);

    // Everything the child reported before it died was durable when reported.
    uint64_t flushes = progress.flushes;
    uint64_t compactions = progress.compactions;
    while (ReadExact(pipe_fds[0], &progress, sizeof(progress))) {
      durable = progress.durable;
      flushes = progress.flushes;
      compactions = progress.compactions;
    }
    close(pipe_fds[0]);
    child_flushes += flushes;
    child_compactions += compactions;

    const std::set<std::string> tables_at_kill = TableFiles(test_dir_);

    std::unique_ptr<DB> parent_db;
    Status open_status = DB::Open(opts, test_dir_, &parent_db);
    ASSERT_TRUE(open_status.ok()) << "Open failed after kill -9 at iteration " << iter << ": "
                                  << open_status.ToString();

    // Open deletes tables no MANIFEST lists: a kill caught one half-written.
    // The background compaction Open starts can also delete some, so this is an estimate.
    const std::set<std::string> tables_after_open = TableFiles(test_dir_);
    for (const auto& table : tables_at_kill) {
      if (tables_after_open.count(table) == 0) {
        ++kills_leaving_orphan_tables;
        break;
      }
    }

    Status verify = model.Verify(parent_db.get(), durable);
    ASSERT_TRUE(verify.ok()) << "After kill -9 at iteration " << iter << ": " << verify.ToString();

    // Compactions keep dying with their process, so L0 would grow until writes
    // stall. Drain it to just under that: the next child then opens with a
    // compaction to run, and the kill can land in the middle of it.
    for (int wait = 0; wait < 5000 && LevelZeroFiles(parent_db.get()) >= kDrainLevelZeroTo;
         ++wait) {
      usleep(1000);
    }
  }

  std::printf("%d kills: child finished %llu flushes and %llu compactions; %d kills left orphans\n",
              kTotalIterations, static_cast<unsigned long long>(child_flushes),
              static_cast<unsigned long long>(child_compactions), kills_leaving_orphan_tables);

  // A shorter run (see STRATA_KILL_ITERATIONS) may not get this far by chance.
  if (kTotalIterations >= 200) {
    EXPECT_GT(child_flushes, 0U) << "kills must land while the child is flushing";
    EXPECT_GT(child_compactions, 0U) << "kills must land while the child is compacting";
    EXPECT_GT(kills_leaving_orphan_tables, 0) << "some kills must interrupt a table being written";
  }

  std::unique_ptr<DB> final_db;
  ASSERT_TRUE(DB::Open(opts, test_dir_, &final_db).ok());
  Status verify = model.Verify(final_db.get(), model.applied());
  EXPECT_TRUE(verify.ok()) << verify.ToString();
}

}  // namespace
}  // namespace strata

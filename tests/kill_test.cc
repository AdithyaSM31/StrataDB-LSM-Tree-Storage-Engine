#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "strata/db.h"
#include "strata/env.h"
#include "strata/options.h"
#include "strata/status.h"

namespace strata {
namespace {

std::string KeyStr(uint64_t i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "k_%08lu", static_cast<unsigned long>(i));
  return std::string(buf);
}

std::string ValStr(uint64_t i) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "v_kill_test_payload_%08lu", static_cast<unsigned long>(i));
  return std::string(buf);
}

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

TEST_F(KillTest, ThousandKillIterationsSurviveWithoutDataLoss) {
  const char* env_iter = std::getenv("STRATA_KILL_ITERATIONS");
  const int kTotalIterations =
      (env_iter != nullptr && std::atoi(env_iter) > 0) ? std::atoi(env_iter) : 1000;

  Options opts;
  opts.create_if_missing = true;
  opts.write_buffer_size = 64 * 1024;

  // Initialize the database cleanly once
  {
    std::unique_ptr<DB> init_db;
    ASSERT_TRUE(DB::Open(opts, test_dir_, &init_db).ok());
    ASSERT_TRUE(init_db->Put(WriteOptions(), KeyStr(0), ValStr(0)).ok());
  }

  opts.create_if_missing = false;
  uint64_t next_key_idx = 1;

  for (int iter = 0; iter < kTotalIterations; ++iter) {
    int pipe_fds[2];
    ASSERT_EQ(pipe(pipe_fds), 0);

    const pid_t pid = fork();
    ASSERT_GE(pid, 0) << "fork failed at iteration " << iter;

    if (pid == 0) {
      // Child process: close read end
      close(pipe_fds[0]);

      std::unique_ptr<DB> child_db;
      Status s = DB::Open(opts, test_dir_, &child_db);
      if (!s.ok()) {
        close(pipe_fds[1]);
        _exit(1);
      }

      // Perform a batch of writes, syncing periodically
      uint64_t cur_key = next_key_idx;
      while (true) {
        WriteOptions wopts;
        const bool sync = (cur_key % 3 == 0);
        wopts.sync = sync;
        s = child_db->Put(wopts, KeyStr(cur_key), ValStr(cur_key));
        if (!s.ok()) {
          break;
        }
        if (sync) {
          ssize_t bytes_written = write(pipe_fds[1], &cur_key, sizeof(cur_key));
          (void)bytes_written;
        }
        cur_key++;
      }
      _exit(0);
    }

    // Parent process: close write end
    close(pipe_fds[1]);

    std::vector<uint64_t> confirmed_syncs;

    // Wait until child has performed at least one synced write
    uint64_t first_sync = 0;
    if (ReadExact(pipe_fds[0], &first_sync, sizeof(first_sync))) {
      confirmed_syncs.push_back(first_sync);
    }

    // Add jitter before killing
    if (iter % 3 != 0) {
      usleep(50 * (iter % 8));
    }

    // Send SIGKILL (kill -9)
    ASSERT_EQ(kill(pid, SIGKILL), 0);

    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), SIGKILL);

    // Read remaining confirmed synced keys from the child
    while (true) {
      uint64_t synced_idx = 0;
      if (ReadExact(pipe_fds[0], &synced_idx, sizeof(synced_idx))) {
        confirmed_syncs.push_back(synced_idx);
      } else {
        break;
      }
    }
    close(pipe_fds[0]);

    // Parent opens database and verifies integrity
    std::unique_ptr<DB> parent_db;
    opts.create_if_missing = false;
    Status open_status = DB::Open(opts, test_dir_, &parent_db);
    ASSERT_TRUE(open_status.ok()) << "Open failed after kill -9 at iteration " << iter << ": "
                                  << open_status.ToString();

    // Verify all confirmed synced keys exist
    for (uint64_t k : confirmed_syncs) {
      std::string val;
      Status s = parent_db->Get(ReadOptions(), KeyStr(k), &val);
      ASSERT_TRUE(s.ok()) << "Synced key " << KeyStr(k) << " missing after kill at iter " << iter;
      EXPECT_EQ(val, ValStr(k));
    }

    // Verify prefix property: all keys up to highest recovered key must exist
    uint64_t highest_key = next_key_idx;
    while (true) {
      std::string val;
      Status s = parent_db->Get(ReadOptions(), KeyStr(highest_key), &val);
      if (s.ok()) {
        EXPECT_EQ(val, ValStr(highest_key));
        highest_key++;
      } else {
        break;
      }
    }

    if (!confirmed_syncs.empty()) {
      EXPECT_GE(highest_key, confirmed_syncs.back() + 1);
    }

    next_key_idx = highest_key;
    parent_db.reset();
  }

  // Final check: reopen one last time and verify entire history
  std::unique_ptr<DB> final_db;
  opts.create_if_missing = false;
  ASSERT_TRUE(DB::Open(opts, test_dir_, &final_db).ok());
  for (uint64_t i = 0; i < next_key_idx; ++i) {
    std::string val;
    Status s = final_db->Get(ReadOptions(), KeyStr(i), &val);
    ASSERT_TRUE(s.ok()) << "Key " << KeyStr(i) << " missing at final verification";
    EXPECT_EQ(val, ValStr(i));
  }
}

}  // namespace
}  // namespace strata

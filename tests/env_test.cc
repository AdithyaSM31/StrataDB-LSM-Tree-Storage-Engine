#include "strata/env.h"

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include "util/fault_injection_env.h"
#include "util/random.h"

namespace strata {
namespace {

// Wrappers take ownership only through std::unique_ptr, never a raw pointer.
static_assert(!std::is_constructible_v<WritableFileWrapper, WritableFile*>);
static_assert(!std::is_constructible_v<SequentialFileWrapper, SequentialFile*>);
static_assert(!std::is_constructible_v<RandomAccessFileWrapper, RandomAccessFile*>);

class EnvTest : public ::testing::Test {
 protected:
  void SetUp() override {
    env_ = Env::Default();
    static std::atomic<uint64_t> test_counter{0};
    test_dir_ =
        (std::filesystem::temp_directory_path() /
         ("stratadb_env_test_" + std::to_string(::getpid()) + "_" +
          std::to_string(env_->NowMicros()) + "_" + std::to_string(test_counter.fetch_add(1))))
            .string();
    std::error_code ec;
    std::filesystem::create_directories(test_dir_, ec);
    ASSERT_FALSE(ec) << ec.message();
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(test_dir_, ec);
  }

  std::string Path(const std::string& name) const {
    return (std::filesystem::path(test_dir_) / name).string();
  }

  Env* env_ = nullptr;
  std::string test_dir_;
};

TEST_F(EnvTest, DefaultEnvSingleton) {
  Env* env1 = Env::Default();
  Env* env2 = Env::Default();
  EXPECT_NE(env1, nullptr);
  EXPECT_EQ(env1, env2);
}

TEST_F(EnvTest, RoundTripWriteAndRead) {
  constexpr size_t kTotalBytes = size_t{1024} * 1024;  // 1 MB
  std::string data;
  data.reserve(kTotalBytes);
  Random rnd(1337);
  while (data.size() < kTotalBytes) {
    uint64_t val = rnd.Next();
    size_t to_add = std::min(sizeof(val), kTotalBytes - data.size());
    data.append(reinterpret_cast<const char*>(&val), to_add);
  }

  std::string file_path = Path("roundtrip.dat");
  std::unique_ptr<WritableFile> writable;
  ASSERT_TRUE(env_->NewWritableFile(file_path, &writable).ok());

  // Write in random-sized chunks, explicitly including writes larger than 64 KB.
  size_t bytes_written = 0;
  bool saw_large_write = false;
  while (bytes_written < kTotalBytes) {
    size_t chunk_size = 0;
    uint32_t choice = rnd.Uniform(10);
    if (choice < 4) {
      // Small write: 1 to 256 bytes
      chunk_size = 1 + rnd.Uniform(256);
    } else if (choice < 7) {
      // Medium write: 1 KB to 32 KB
      chunk_size = 1024 + rnd.Uniform(32768);
    } else {
      // Large write: 65 KB to 128 KB (bypasses 64 KB buffer)
      chunk_size = 65536 + rnd.Uniform(65536);
      saw_large_write = true;
    }
    chunk_size = std::min(chunk_size, kTotalBytes - bytes_written);
    ASSERT_TRUE(writable->Append(std::string_view(data.data() + bytes_written, chunk_size)).ok());
    bytes_written += chunk_size;
  }
  EXPECT_TRUE(saw_large_write);

  ASSERT_TRUE(writable->Flush().ok());
  ASSERT_TRUE(writable->Sync().ok());
  ASSERT_TRUE(writable->Close().ok());

  uint64_t file_size = 0;
  ASSERT_TRUE(env_->GetFileSize(file_path, &file_size).ok());
  EXPECT_EQ(file_size, kTotalBytes);

  // Read back sequentially using random-sized reads.
  std::unique_ptr<SequentialFile> seq_file;
  ASSERT_TRUE(env_->NewSequentialFile(file_path, &seq_file).ok());
  std::string seq_read_back;
  seq_read_back.reserve(kTotalBytes);
  std::vector<char> scratch(131072);  // 128 KB buffer
  while (seq_read_back.size() < kTotalBytes) {
    size_t to_read = 1 + rnd.Uniform(65536);
    to_read = std::min(to_read, scratch.size());
    std::string_view chunk;
    ASSERT_TRUE(seq_file->Read(to_read, &chunk, scratch.data()).ok());
    if (chunk.empty()) {
      break;
    }
    seq_read_back.append(chunk.data(), chunk.size());
  }
  EXPECT_EQ(seq_read_back.size(), kTotalBytes);
  EXPECT_EQ(seq_read_back, data);

  // Read back randomly using RandomAccessFile at random offsets.
  std::unique_ptr<RandomAccessFile> rnd_file;
  ASSERT_TRUE(env_->NewRandomAccessFile(file_path, &rnd_file).ok());
  for (int i = 0; i < 500; ++i) {
    uint64_t offset = rnd.Uniform(static_cast<uint32_t>(kTotalBytes));
    size_t n = 1 + rnd.Uniform(32768);
    n = std::min(n, scratch.size());
    std::string_view result;
    ASSERT_TRUE(rnd_file->Read(offset, n, &result, scratch.data()).ok());
    size_t expected_len = std::min(static_cast<uint64_t>(n), kTotalBytes - offset);
    EXPECT_EQ(result.size(), expected_len);
    EXPECT_EQ(result, std::string_view(data.data() + offset, expected_len));
  }
}

TEST_F(EnvTest, ReadPastEndOfFile) {
  std::string file_path = Path("short.dat");
  std::unique_ptr<WritableFile> writable;
  ASSERT_TRUE(env_->NewWritableFile(file_path, &writable).ok());
  ASSERT_TRUE(writable->Append("0123456789").ok());
  ASSERT_TRUE(writable->Close().ok());

  // SequentialFile reading past end
  std::unique_ptr<SequentialFile> seq_file;
  ASSERT_TRUE(env_->NewSequentialFile(file_path, &seq_file).ok());
  char scratch[64];
  std::string_view result;
  ASSERT_TRUE(seq_file->Read(10, &result, scratch).ok());
  EXPECT_EQ(result, "0123456789");

  // At EOF: returns OK and empty result
  ASSERT_TRUE(seq_file->Read(10, &result, scratch).ok());
  EXPECT_TRUE(result.empty());

  // RandomAccessFile reading past end
  std::unique_ptr<RandomAccessFile> rnd_file;
  ASSERT_TRUE(env_->NewRandomAccessFile(file_path, &rnd_file).ok());

  // Read exactly up to end
  ASSERT_TRUE(rnd_file->Read(0, 10, &result, scratch).ok());
  EXPECT_EQ(result, "0123456789");

  // Read starting before end and requesting past end (short read)
  ASSERT_TRUE(rnd_file->Read(5, 20, &result, scratch).ok());
  EXPECT_EQ(result, "56789");

  // Read starting exactly at end (empty read)
  ASSERT_TRUE(rnd_file->Read(10, 20, &result, scratch).ok());
  EXPECT_TRUE(result.empty());

  // Read starting well past end (empty read)
  ASSERT_TRUE(rnd_file->Read(100, 20, &result, scratch).ok());
  EXPECT_TRUE(result.empty());
}

TEST_F(EnvTest, MissingFileReturnsNotFound) {
  std::string missing = Path("non_existent_file.dat");

  std::unique_ptr<SequentialFile> seq;
  Status s1 = env_->NewSequentialFile(missing, &seq);
  EXPECT_FALSE(s1.ok());
  EXPECT_TRUE(s1.IsNotFound());
  EXPECT_EQ(seq, nullptr);

  std::unique_ptr<RandomAccessFile> rnd;
  Status s2 = env_->NewRandomAccessFile(missing, &rnd);
  EXPECT_FALSE(s2.ok());
  EXPECT_TRUE(s2.IsNotFound());
  EXPECT_EQ(rnd, nullptr);

  uint64_t size = 0;
  Status s3 = env_->GetFileSize(missing, &size);
  EXPECT_FALSE(s3.ok());
  EXPECT_TRUE(s3.IsNotFound());

  Status s4 = env_->RemoveFile(missing);
  EXPECT_FALSE(s4.ok());
  EXPECT_TRUE(s4.IsNotFound());
}

TEST_F(EnvTest, RenameFileOverwritesExisting) {
  std::string src = Path("src.txt");
  std::string target = Path("target.txt");

  std::unique_ptr<WritableFile> f_src;
  ASSERT_TRUE(env_->NewWritableFile(src, &f_src).ok());
  ASSERT_TRUE(f_src->Append("source content").ok());
  ASSERT_TRUE(f_src->Close().ok());

  std::unique_ptr<WritableFile> f_target;
  ASSERT_TRUE(env_->NewWritableFile(target, &f_target).ok());
  ASSERT_TRUE(f_target->Append("target old content").ok());
  ASSERT_TRUE(f_target->Close().ok());

  ASSERT_TRUE(env_->RenameFile(src, target).ok());
  EXPECT_FALSE(env_->FileExists(src));
  EXPECT_TRUE(env_->FileExists(target));

  std::unique_ptr<SequentialFile> seq;
  ASSERT_TRUE(env_->NewSequentialFile(target, &seq).ok());
  char scratch[64];
  std::string_view result;
  ASSERT_TRUE(seq->Read(64, &result, scratch).ok());
  EXPECT_EQ(result, "source content");

  // Renaming non-existent source fails with NotFound
  Status s = env_->RenameFile(src, target);
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.IsNotFound());
}

TEST_F(EnvTest, GetChildrenListsFiles) {
  std::string f1 = Path("alpha.log");
  std::string f2 = Path("beta.sst");
  std::string sub = Path("subdir");

  std::unique_ptr<WritableFile> w1, w2;
  ASSERT_TRUE(env_->NewWritableFile(f1, &w1).ok());
  ASSERT_TRUE(w1->Close().ok());
  ASSERT_TRUE(env_->NewWritableFile(f2, &w2).ok());
  ASSERT_TRUE(w2->Close().ok());
  ASSERT_TRUE(env_->CreateDir(sub).ok());

  std::vector<std::string> children;
  ASSERT_TRUE(env_->GetChildren(test_dir_, &children).ok());

  // Must not contain "." or ".."
  EXPECT_EQ(std::find(children.begin(), children.end(), "."), children.end());
  EXPECT_EQ(std::find(children.begin(), children.end(), ".."), children.end());

  std::sort(children.begin(), children.end());
  EXPECT_EQ(children, std::vector<std::string>({"alpha.log", "beta.sst", "subdir"}));

  // Missing directory
  Status s = env_->GetChildren(Path("no_such_dir"), &children);
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.IsNotFound());
}

TEST_F(EnvTest, RemoveFileAndGetFileSize) {
  std::string file_path = Path("sized_file.dat");
  std::string content(4096, 'x');

  std::unique_ptr<WritableFile> w;
  ASSERT_TRUE(env_->NewWritableFile(file_path, &w).ok());
  ASSERT_TRUE(w->Append(content).ok());
  ASSERT_TRUE(w->Close().ok());

  EXPECT_TRUE(env_->FileExists(file_path));
  uint64_t size = 0;
  ASSERT_TRUE(env_->GetFileSize(file_path, &size).ok());
  EXPECT_EQ(size, 4096);

  ASSERT_TRUE(env_->RemoveFile(file_path).ok());
  EXPECT_FALSE(env_->FileExists(file_path));

  // Second removal returns NotFound
  Status s = env_->RemoveFile(file_path);
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.IsNotFound());
}

TEST_F(EnvTest, LockingSamePathFailsAndUnlockAllowsRelock) {
  std::string lock_path = Path("LOCK");
  std::unique_ptr<FileLock> lock1;
  ASSERT_TRUE(env_->LockFile(lock_path, &lock1).ok());
  EXPECT_NE(lock1, nullptr);

  // Second lock attempt in the same process must fail.
  std::unique_ptr<FileLock> lock2;
  Status s = env_->LockFile(lock_path, &lock2);
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.IsIOError());
  EXPECT_EQ(lock2, nullptr);

  // Fork child process to confirm lock conflict across processes.
  pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    std::unique_ptr<FileLock> child_lock;
    Status child_status = env_->LockFile(lock_path, &child_lock);
    if (child_status.ok()) {
      ::_exit(1);  // Lock should have failed!
    }
    ::_exit(0);  // Expected failure.
  }
  int child_exit = 0;
  ASSERT_EQ(::waitpid(pid, &child_exit, 0), pid);
  ASSERT_TRUE(WIFEXITED(child_exit));
  EXPECT_EQ(WEXITSTATUS(child_exit), 0);

  // Unlock lock1. The LOCK file itself must not be deleted.
  ASSERT_TRUE(env_->UnlockFile(std::move(lock1)).ok());
  EXPECT_TRUE(env_->FileExists(lock_path));

  // Relocking must now succeed.
  std::unique_ptr<FileLock> lock3;
  ASSERT_TRUE(env_->LockFile(lock_path, &lock3).ok());
  EXPECT_NE(lock3, nullptr);

  // Unlocking again leaves the LOCK file present on disk.
  ASSERT_TRUE(env_->UnlockFile(std::move(lock3)).ok());
  EXPECT_TRUE(env_->FileExists(lock_path));
}

TEST_F(EnvTest, SyncDir) {
  // Syncing a real directory succeeds.
  ASSERT_TRUE(env_->SyncDir(test_dir_).ok());

  std::string sub = Path("sync_sub");
  ASSERT_TRUE(env_->CreateDir(sub).ok());
  ASSERT_TRUE(env_->SyncDir(sub).ok());

  // Syncing a missing directory returns an error.
  std::string missing = Path("missing_dir");
  Status s = env_->SyncDir(missing);
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.IsNotFound());
}

TEST_F(EnvTest, ConcurrentReads) {
  constexpr size_t kFileSize = size_t{512} * 1024;  // 512 KB
  std::string data;
  data.reserve(kFileSize);
  Random rnd(999);
  while (data.size() < kFileSize) {
    uint64_t val = rnd.Next();
    size_t to_add = std::min(sizeof(val), kFileSize - data.size());
    data.append(reinterpret_cast<const char*>(&val), to_add);
  }

  std::string file_path = Path("concurrent.dat");
  std::unique_ptr<WritableFile> w;
  ASSERT_TRUE(env_->NewWritableFile(file_path, &w).ok());
  ASSERT_TRUE(w->Append(data).ok());
  ASSERT_TRUE(w->Close().ok());

  std::unique_ptr<RandomAccessFile> r_file;
  ASSERT_TRUE(env_->NewRandomAccessFile(file_path, &r_file).ok());

  // Spawn 4 threads reading from the single RandomAccessFile instance.
  constexpr int kNumThreads = 4;
  constexpr int kReadsPerThread = 200;
  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);

  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([&, seed = 1000 + t]() {
      Random thread_rnd(seed);
      std::vector<char> scratch(8192);
      for (int i = 0; i < kReadsPerThread; ++i) {
        uint64_t offset = thread_rnd.Uniform(static_cast<uint32_t>(kFileSize));
        size_t n = 1 + thread_rnd.Uniform(static_cast<uint32_t>(scratch.size()));
        std::string_view result;
        Status s = r_file->Read(offset, n, &result, scratch.data());
        ASSERT_TRUE(s.ok());
        size_t expected_len = std::min(static_cast<uint64_t>(n), kFileSize - offset);
        ASSERT_EQ(result.size(), expected_len);
        ASSERT_EQ(result, std::string_view(data.data() + offset, expected_len));
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }
}

TEST_F(EnvTest, SequentialFileSkip) {
  std::string file_path = Path("skip_test.dat");
  std::unique_ptr<WritableFile> w;
  ASSERT_TRUE(env_->NewWritableFile(file_path, &w).ok());
  ASSERT_TRUE(w->Append("abcdefghijklmnopqrstuvwxyz").ok());
  ASSERT_TRUE(w->Close().ok());

  std::unique_ptr<SequentialFile> seq;
  ASSERT_TRUE(env_->NewSequentialFile(file_path, &seq).ok());

  char scratch[16];
  std::string_view result;
  ASSERT_TRUE(seq->Read(5, &result, scratch).ok());
  EXPECT_EQ(result, "abcde");

  ASSERT_TRUE(seq->Skip(5).ok());  // skip fghij

  ASSERT_TRUE(seq->Read(5, &result, scratch).ok());
  EXPECT_EQ(result, "klmno");
}

TEST_F(EnvTest, WritableFileClosedRejectsOperations) {
  std::string file_path = Path("closed_test.dat");
  std::unique_ptr<WritableFile> w;
  ASSERT_TRUE(env_->NewWritableFile(file_path, &w).ok());
  ASSERT_TRUE(w->Append("initial").ok());
  ASSERT_TRUE(w->Close().ok());

  // Repeated close is idempotent OK
  EXPECT_TRUE(w->Close().ok());

  // Append, Flush, Sync after close must fail
  EXPECT_TRUE(w->Append("more").IsIOError());
  EXPECT_TRUE(w->Flush().IsIOError());
  EXPECT_TRUE(w->Sync().IsIOError());
}

TEST_F(EnvTest, EnvWrapperDelegation) {
  EnvWrapper wrapper(env_);
  EXPECT_EQ(wrapper.target(), env_);

  std::string dir = Path("wrapper_dir");
  ASSERT_TRUE(wrapper.CreateDir(dir).ok());
  EXPECT_TRUE(wrapper.FileExists(dir));
  EXPECT_TRUE(wrapper.SyncDir(dir).ok());

  std::string file = Path("wrapper_dir/f.txt");
  std::unique_ptr<WritableFile> w;
  ASSERT_TRUE(wrapper.NewWritableFile(file, &w).ok());
  ASSERT_TRUE(w->Append("wrapper data").ok());
  ASSERT_TRUE(w->Close().ok());

  uint64_t sz = 0;
  ASSERT_TRUE(wrapper.GetFileSize(file, &sz).ok());
  EXPECT_EQ(sz, 12);

  std::vector<std::string> children;
  ASSERT_TRUE(wrapper.GetChildren(dir, &children).ok());
  EXPECT_EQ(children, std::vector<std::string>({"f.txt"}));

  ASSERT_TRUE(wrapper.RemoveFile(file).ok());
  EXPECT_FALSE(wrapper.FileExists(file));
  ASSERT_TRUE(wrapper.RemoveDir(dir).ok());
  EXPECT_FALSE(wrapper.FileExists(dir));
}

TEST_F(EnvTest, NowMicrosAndSleep) {
  uint64_t t1 = env_->NowMicros();
  env_->SleepForMicroseconds(10000);  // 10 ms
  uint64_t t2 = env_->NowMicros();
  EXPECT_GE(t2, t1);
  // Monotonic steady clock should have advanced by at least several milliseconds
  EXPECT_GE(t2 - t1, 5000);
}

TEST_F(EnvTest, DirectoryOperationsEdgeCases) {
  std::string dir = Path("edge_dir");
  ASSERT_TRUE(env_->CreateDir(dir).ok());

  // Creating an existing directory fails with IOError
  Status s1 = env_->CreateDir(dir);
  EXPECT_FALSE(s1.ok());
  EXPECT_TRUE(s1.IsIOError());

  // Create a file inside dir
  std::string child_file = Path("edge_dir/child.txt");
  std::unique_ptr<WritableFile> w;
  ASSERT_TRUE(env_->NewWritableFile(child_file, &w).ok());
  ASSERT_TRUE(w->Close().ok());

  // Removing non-empty directory must fail with IOError
  Status s2 = env_->RemoveDir(dir);
  EXPECT_FALSE(s2.ok());
  EXPECT_TRUE(s2.IsIOError());

  // RemoveFile on directory must fail with InvalidArgument
  Status s3 = env_->RemoveFile(dir);
  EXPECT_FALSE(s3.ok());
  EXPECT_TRUE(s3.IsInvalidArgument());

  // RemoveDir on regular file must fail with InvalidArgument
  Status s4 = env_->RemoveDir(child_file);
  EXPECT_FALSE(s4.ok());
  EXPECT_TRUE(s4.IsInvalidArgument());

  // Remove file, then RemoveDir succeeds
  ASSERT_TRUE(env_->RemoveFile(child_file).ok());
  ASSERT_TRUE(env_->RemoveDir(dir).ok());
  EXPECT_FALSE(env_->FileExists(dir));

  // Second RemoveDir fails with NotFound
  Status s5 = env_->RemoveDir(dir);
  EXPECT_FALSE(s5.ok());
  EXPECT_TRUE(s5.IsNotFound());
}

TEST_F(EnvTest, EmptyFileOperations) {
  std::string empty_file = Path("empty.dat");
  std::unique_ptr<WritableFile> w;
  ASSERT_TRUE(env_->NewWritableFile(empty_file, &w).ok());
  ASSERT_TRUE(w->Close().ok());

  uint64_t size = 999;
  ASSERT_TRUE(env_->GetFileSize(empty_file, &size).ok());
  EXPECT_EQ(size, 0);

  std::unique_ptr<SequentialFile> seq;
  ASSERT_TRUE(env_->NewSequentialFile(empty_file, &seq).ok());
  char scratch[64];
  std::string_view result;
  ASSERT_TRUE(seq->Read(64, &result, scratch).ok());
  EXPECT_TRUE(result.empty());

  std::unique_ptr<RandomAccessFile> rnd;
  ASSERT_TRUE(env_->NewRandomAccessFile(empty_file, &rnd).ok());
  ASSERT_TRUE(rnd->Read(0, 64, &result, scratch).ok());
  EXPECT_TRUE(result.empty());
}

TEST_F(EnvTest, NullArgumentValidation) {
  std::string file = Path("null_test.dat");
  EXPECT_TRUE(env_->NewSequentialFile(file, nullptr).IsInvalidArgument());
  EXPECT_TRUE(env_->NewRandomAccessFile(file, nullptr).IsInvalidArgument());
  EXPECT_TRUE(env_->NewWritableFile(file, nullptr).IsInvalidArgument());
  EXPECT_TRUE(env_->GetChildren(test_dir_, nullptr).IsInvalidArgument());
  EXPECT_TRUE(env_->GetFileSize(file, nullptr).IsInvalidArgument());
  EXPECT_TRUE(env_->LockFile(file, nullptr).IsInvalidArgument());
  EXPECT_TRUE(env_->UnlockFile(nullptr).IsInvalidArgument());
}

TEST_F(EnvTest, BufferBoundaryAppendAndFlush) {
  std::string file = Path("boundary.dat");
  std::unique_ptr<WritableFile> w;
  ASSERT_TRUE(env_->NewWritableFile(file, &w).ok());

  // Exactly 64 KB (buffer size)
  std::string chunk_64k(65536, 'A');
  ASSERT_TRUE(w->Append(chunk_64k).ok());

  // 64 KB + 1 byte
  std::string chunk_64k_plus_1(65537, 'B');
  ASSERT_TRUE(w->Append(chunk_64k_plus_1).ok());

  // Flush and Sync
  ASSERT_TRUE(w->Flush().ok());
  ASSERT_TRUE(w->Sync().ok());
  ASSERT_TRUE(w->Close().ok());

  uint64_t sz = 0;
  ASSERT_TRUE(env_->GetFileSize(file, &sz).ok());
  EXPECT_EQ(sz, 65536 + 65537);

  std::unique_ptr<RandomAccessFile> r;
  ASSERT_TRUE(env_->NewRandomAccessFile(file, &r).ok());
  std::vector<char> scratch(65537);
  std::string_view res;

  ASSERT_TRUE(r->Read(0, 65536, &res, scratch.data()).ok());
  EXPECT_EQ(res, chunk_64k);

  ASSERT_TRUE(r->Read(65536, 65537, &res, scratch.data()).ok());
  EXPECT_EQ(res, chunk_64k_plus_1);
}

TEST_F(EnvTest, FsyncgateStickyErrorOnDiskFull) {
  // /dev/full is a standard POSIX device that always returns ENOSPC on write.
  if (!env_->FileExists("/dev/full")) {
    GTEST_SKIP() << "/dev/full is not available on this platform";
  }

  std::unique_ptr<WritableFile> w;
  ASSERT_TRUE(env_->NewWritableFile("/dev/full", &w).ok());

  // Attempting to append and flush data to /dev/full triggers ENOSPC.
  ASSERT_TRUE(w->Append("data to fill buffer").ok());
  Status flush_status = w->Flush();
  EXPECT_FALSE(flush_status.ok());
  EXPECT_TRUE(flush_status.IsIOError());

  // Fsyncgate durability contract: Once a write or sync fails, subsequent calls
  // must immediately fail with the latched sticky error and never attempt retries.
  Status append_after_error = w->Append("subsequent write");
  EXPECT_FALSE(append_after_error.ok());
  EXPECT_TRUE(append_after_error.IsIOError());

  Status sync_after_error = w->Sync();
  EXPECT_FALSE(sync_after_error.ok());
  EXPECT_TRUE(sync_after_error.IsIOError());

  // Close() must NOT swallow the sticky error and return OK. It must propagate the error.
  Status close_status = w->Close();
  EXPECT_FALSE(close_status.ok());
  EXPECT_TRUE(close_status.IsIOError());

  // A repeated Close keeps reporting the failure: once a file has failed it
  // must never look healthy again, even to cleanup code that closes twice.
  Status second_close = w->Close();
  EXPECT_TRUE(second_close.IsIOError());
  EXPECT_EQ(second_close.ToString(), close_status.ToString());
}

TEST_F(EnvTest, ReadBoundaryAndNullScratchValidation) {
  std::string file_path = Path("boundary_reads.dat");
  std::unique_ptr<WritableFile> w;
  ASSERT_TRUE(env_->NewWritableFile(file_path, &w).ok());
  ASSERT_TRUE(w->Append("0123456789").ok());
  ASSERT_TRUE(w->Close().ok());

  std::unique_ptr<RandomAccessFile> rnd;
  ASSERT_TRUE(env_->NewRandomAccessFile(file_path, &rnd).ok());

  char scratch[64];
  std::string_view result;

  // offset exceeding off_t max
  constexpr uint64_t kOverMaxOff = static_cast<uint64_t>(std::numeric_limits<off_t>::max()) + 1ULL;
  EXPECT_TRUE(rnd->Read(kOverMaxOff, 10, &result, scratch).IsInvalidArgument());

  // n exceeding off_t max must not wrap around in unsigned subtraction
  constexpr uint64_t kHugeN = (1ULL << 63) + 100ULL;
  EXPECT_TRUE(rnd->Read(100, kHugeN, &result, scratch).IsInvalidArgument());

  // offset + n exceeding off_t max
  constexpr uint64_t kNearMaxOff = static_cast<uint64_t>(std::numeric_limits<off_t>::max()) - 5ULL;
  EXPECT_TRUE(rnd->Read(kNearMaxOff, 10, &result, scratch).IsInvalidArgument());

  // null scratch with n > 0 must fail with InvalidArgument
  EXPECT_TRUE(rnd->Read(0, 5, &result, nullptr).IsInvalidArgument());

  // null scratch with n == 0 is valid and returns empty result
  ASSERT_TRUE(rnd->Read(0, 0, &result, nullptr).ok());
  EXPECT_TRUE(result.empty());

  // SequentialFile null scratch check
  std::unique_ptr<SequentialFile> seq;
  ASSERT_TRUE(env_->NewSequentialFile(file_path, &seq).ok());
  EXPECT_TRUE(seq->Read(5, &result, nullptr).IsInvalidArgument());
  ASSERT_TRUE(seq->Read(0, &result, nullptr).ok());
  EXPECT_TRUE(result.empty());

  // SequentialFile::Skip overflow check
  EXPECT_TRUE(seq->Skip(kOverMaxOff).IsInvalidArgument());
}

TEST_F(EnvTest, BufferTransitionSmallAndLargeWrites) {
  std::string file_path = Path("mixed_buffer.dat");
  std::unique_ptr<WritableFile> w;
  ASSERT_TRUE(env_->NewWritableFile(file_path, &w).ok());

  // Construct distinct patterns
  std::string chunk_50k(50000, 'X');
  std::string chunk_20k(20000, 'Y');
  std::string chunk_100k(100000, 'Z');
  std::string chunk_30k(30000, 'W');

  std::string full_expected = chunk_50k + chunk_20k + chunk_100k + chunk_30k;

  // 1. Write 50 KB (buffered, 50k / 64k)
  ASSERT_TRUE(w->Append(chunk_50k).ok());
  // 2. Write 20 KB (transitions buffer to full, flushes 64 KB, retains 6 KB in buffer)
  ASSERT_TRUE(w->Append(chunk_20k).ok());
  // 3. Write 100 KB (flushes 6 KB, writes 100 KB directly unbuffered)
  ASSERT_TRUE(w->Append(chunk_100k).ok());
  // 4. Write 30 KB (buffered into empty buffer)
  ASSERT_TRUE(w->Append(chunk_30k).ok());

  ASSERT_TRUE(w->Flush().ok());
  ASSERT_TRUE(w->Sync().ok());
  ASSERT_TRUE(w->Close().ok());

  uint64_t file_size = 0;
  ASSERT_TRUE(env_->GetFileSize(file_path, &file_size).ok());
  EXPECT_EQ(file_size, full_expected.size());

  // Verify byte-for-byte correctness sequentially
  std::unique_ptr<SequentialFile> seq;
  ASSERT_TRUE(env_->NewSequentialFile(file_path, &seq).ok());
  std::vector<char> scratch(full_expected.size());
  std::string_view read_result;
  ASSERT_TRUE(seq->Read(full_expected.size(), &read_result, scratch.data()).ok());
  EXPECT_EQ(read_result, full_expected);
}

TEST_F(EnvTest, FileWrappersDelegationAndOwnership) {
  std::string file_path = Path("wrapper_file.dat");

  // Test WritableFileWrapper with unique_ptr ownership
  std::unique_ptr<WritableFile> base_w;
  ASSERT_TRUE(env_->NewWritableFile(file_path, &base_w).ok());
  WritableFile* raw_base_w = base_w.get();

  auto w_wrapper = std::make_unique<WritableFileWrapper>(std::move(base_w));
  EXPECT_EQ(w_wrapper->target(), raw_base_w);
  ASSERT_TRUE(w_wrapper->Append("hello wrapper").ok());
  ASSERT_TRUE(w_wrapper->Flush().ok());
  ASSERT_TRUE(w_wrapper->Sync().ok());
  ASSERT_TRUE(w_wrapper->Close().ok());
  w_wrapper.reset();  // Should cleanly destroy underlying WritableFile

  // Test SequentialFileWrapper
  std::unique_ptr<SequentialFile> base_seq;
  ASSERT_TRUE(env_->NewSequentialFile(file_path, &base_seq).ok());
  SequentialFile* raw_base_seq = base_seq.get();

  auto seq_wrapper = std::make_unique<SequentialFileWrapper>(std::move(base_seq));
  EXPECT_EQ(seq_wrapper->target(), raw_base_seq);
  char scratch[64];
  std::string_view res;
  ASSERT_TRUE(seq_wrapper->Read(5, &res, scratch).ok());
  EXPECT_EQ(res, "hello");
  ASSERT_TRUE(seq_wrapper->Skip(1).ok());
  ASSERT_TRUE(seq_wrapper->Read(7, &res, scratch).ok());
  EXPECT_EQ(res, "wrapper");
  seq_wrapper.reset();

  // Test RandomAccessFileWrapper
  std::unique_ptr<RandomAccessFile> base_rnd;
  ASSERT_TRUE(env_->NewRandomAccessFile(file_path, &base_rnd).ok());
  RandomAccessFile* raw_base_rnd = base_rnd.get();

  auto rnd_wrapper = std::make_unique<RandomAccessFileWrapper>(std::move(base_rnd));
  EXPECT_EQ(rnd_wrapper->target(), raw_base_rnd);
  ASSERT_TRUE(rnd_wrapper->Read(6, 7, &res, scratch).ok());
  EXPECT_EQ(res, "wrapper");
  rnd_wrapper.reset();
}

TEST_F(EnvTest, FaultInjectionCrashLosesUnsyncedData) {
  FaultInjectionEnv f_env(env_);
  const std::string file_path = Path("fault_test_file.txt");

  std::unique_ptr<WritableFile> wfile;
  ASSERT_TRUE(f_env.NewWritableFile(file_path, &wfile).ok());
  ASSERT_TRUE(wfile->Append("synced_prefix").ok());
  ASSERT_TRUE(wfile->Sync().ok());
  ASSERT_TRUE(f_env.SyncDir(test_dir_).ok());

  ASSERT_TRUE(wfile->Append("_unsynced_tail").ok());
  // Do NOT sync the second part
  ASSERT_TRUE(wfile->Close().ok());
  wfile.reset();

  // Simulate crash
  ASSERT_TRUE(f_env.SimulateCrash().ok());
  f_env.ResumeAfterCrash();

  // Check file contents: must have truncated to synced_prefix
  std::unique_ptr<SequentialFile> rfile;
  ASSERT_TRUE(f_env.NewSequentialFile(file_path, &rfile).ok());
  char scratch[64];
  std::string_view read_result;
  ASSERT_TRUE(rfile->Read(64, &read_result, scratch).ok());
  EXPECT_EQ(read_result, "synced_prefix");
}

TEST_F(EnvTest, FaultInjectionCrashStopsTheFilesystem) {
  FaultInjectionEnv f_env(env_);
  const std::string file_path = Path("stopped.txt");

  std::unique_ptr<WritableFile> wfile;
  ASSERT_TRUE(f_env.NewWritableFile(file_path, &wfile).ok());
  ASSERT_TRUE(f_env.SimulateCrash().ok());

  EXPECT_TRUE(wfile->Append("x").IsIOError());
  std::unique_ptr<WritableFile> other;
  EXPECT_TRUE(f_env.NewWritableFile(Path("other.txt"), &other).IsIOError());
  EXPECT_TRUE(f_env.SyncDir(test_dir_).IsIOError());

  f_env.ResumeAfterCrash();
  EXPECT_TRUE(f_env.NewWritableFile(Path("other.txt"), &other).ok());
}

TEST_F(EnvTest, FaultInjectionCrashLosesFileWithoutDirectorySync) {
  FaultInjectionEnv f_env(env_);
  const std::string synced_path = Path("dir_synced.txt");
  const std::string unsynced_path = Path("dir_unsynced.txt");

  std::unique_ptr<WritableFile> wfile;
  ASSERT_TRUE(f_env.NewWritableFile(synced_path, &wfile).ok());
  ASSERT_TRUE(wfile->Sync().ok());
  ASSERT_TRUE(f_env.SyncDir(test_dir_).ok());

  // Its data is synced, but nothing has made its directory entry durable.
  std::unique_ptr<WritableFile> wfile2;
  ASSERT_TRUE(f_env.NewWritableFile(unsynced_path, &wfile2).ok());
  ASSERT_TRUE(wfile2->Append("data").ok());
  ASSERT_TRUE(wfile2->Sync().ok());

  ASSERT_TRUE(f_env.SimulateCrash().ok());
  f_env.ResumeAfterCrash();

  EXPECT_TRUE(f_env.FileExists(synced_path));
  EXPECT_FALSE(f_env.FileExists(unsynced_path));
}

TEST_F(EnvTest, FaultInjectionCrashUndoesRenameWithoutDirectorySync) {
  FaultInjectionEnv f_env(env_);
  const std::string target = Path("rename_target.txt");
  const std::string src = Path("rename_src.txt");

  auto write_file = [&](const std::string& path, const std::string& contents) {
    std::unique_ptr<WritableFile> wfile;
    ASSERT_TRUE(f_env.NewWritableFile(path, &wfile).ok());
    ASSERT_TRUE(wfile->Append(contents).ok());
    ASSERT_TRUE(wfile->Sync().ok());
    ASSERT_TRUE(wfile->Close().ok());
  };
  auto read_file = [&](const std::string& path) {
    std::unique_ptr<SequentialFile> rfile;
    EXPECT_TRUE(f_env.NewSequentialFile(path, &rfile).ok());
    char scratch[64];
    std::string_view result;
    EXPECT_TRUE(rfile->Read(sizeof(scratch), &result, scratch).ok());
    return std::string(result);
  };

  write_file(target, "old");
  ASSERT_TRUE(f_env.SyncDir(test_dir_).ok());

  write_file(src, "new");
  ASSERT_TRUE(f_env.RenameFile(src, target).ok());
  EXPECT_EQ(read_file(target), "new");
  ASSERT_TRUE(f_env.SimulateCrash().ok());
  f_env.ResumeAfterCrash();
  EXPECT_EQ(read_file(target), "old");

  // With the directory synced, the rename is durable.
  write_file(src, "new");
  ASSERT_TRUE(f_env.RenameFile(src, target).ok());
  ASSERT_TRUE(f_env.SyncDir(test_dir_).ok());
  ASSERT_TRUE(f_env.SimulateCrash().ok());
  f_env.ResumeAfterCrash();
  EXPECT_EQ(read_file(target), "new");
}

TEST_F(EnvTest, FaultInjectionCrashCanKeepATornTail) {
  FaultInjectionEnv f_env(env_);
  const std::string file_path = Path("torn.txt");
  const std::string tail(1000, 'x');

  std::set<uint64_t> sizes;
  for (uint32_t seed = 0; seed < 20; ++seed) {
    std::unique_ptr<WritableFile> wfile;
    ASSERT_TRUE(f_env.NewWritableFile(file_path, &wfile).ok());
    ASSERT_TRUE(wfile->Append("synced").ok());
    ASSERT_TRUE(wfile->Sync().ok());
    ASSERT_TRUE(f_env.SyncDir(test_dir_).ok());
    ASSERT_TRUE(wfile->Append(tail).ok());
    ASSERT_TRUE(f_env.SimulateCrash(FaultInjectionEnv::UnsyncedData::kKeepRandomTail, seed).ok());
    f_env.ResumeAfterCrash();

    uint64_t size = 0;
    ASSERT_TRUE(f_env.GetFileSize(file_path, &size).ok());
    EXPECT_GE(size, 6U);
    EXPECT_LE(size, 6U + tail.size());
    sizes.insert(size);
  }
  EXPECT_GT(sizes.size(), 1U) << "the tail should be cut at different places";
}

TEST_F(EnvTest, FaultInjectionCrashesAtTheChosenOperation) {
  FaultInjectionEnv f_env(env_);
  f_env.SetCrashAtOperation(3);

  std::unique_ptr<WritableFile> wfile;
  ASSERT_TRUE(f_env.NewWritableFile(Path("crash_at.txt"), &wfile).ok());  // operation 1
  ASSERT_TRUE(wfile->Append("a").ok());                                   // operation 2
  EXPECT_TRUE(f_env.IsFilesystemActive());
  EXPECT_TRUE(wfile->Sync().IsIOError());  // operation 3: the crash
  EXPECT_FALSE(f_env.IsFilesystemActive());
  EXPECT_TRUE(wfile->Append("b").IsIOError());

  ASSERT_TRUE(f_env.SimulateCrash().ok());
  f_env.ResumeAfterCrash();
  EXPECT_TRUE(f_env.IsFilesystemActive());
}

TEST_F(EnvTest, FaultInjectionFailedManifestSyncKeepsItsBytes) {
  FaultInjectionEnv f_env(env_);
  const std::string manifest = Path("MANIFEST-000001");
  const std::string other = Path("000002.sst");

  std::unique_ptr<WritableFile> manifest_file;
  std::unique_ptr<WritableFile> other_file;
  ASSERT_TRUE(f_env.NewWritableFile(manifest, &manifest_file).ok());
  ASSERT_TRUE(f_env.NewWritableFile(other, &other_file).ok());

  f_env.SetFailManifestSyncs(true);
  ASSERT_TRUE(manifest_file->Append("record").ok());
  EXPECT_TRUE(manifest_file->Sync().IsIOError());
  EXPECT_TRUE(other_file->Sync().ok());

  uint64_t size = 0;
  ASSERT_TRUE(f_env.GetFileSize(manifest, &size).ok());
  EXPECT_EQ(size, 6U);
}

TEST_F(EnvTest, FaultInjectionErrorInjection) {
  FaultInjectionEnv f_env(env_);
  const std::string file_path = Path("fault_inject_file.txt");

  // Test NewWritableFile error
  f_env.SetFailNewWritableFile(true);
  std::unique_ptr<WritableFile> wfile;
  EXPECT_TRUE(f_env.NewWritableFile(file_path, &wfile).IsIOError());
  f_env.SetFailNewWritableFile(false);

  // Successfully open file
  ASSERT_TRUE(f_env.NewWritableFile(file_path, &wfile).ok());

  // Test write error on Append and Flush
  f_env.SetFailWrites(true);
  EXPECT_TRUE(wfile->Append("hello").IsIOError());
  EXPECT_TRUE(wfile->Flush().IsIOError());
  f_env.SetFailWrites(false);
  ASSERT_TRUE(wfile->Append("hello").ok());
  ASSERT_TRUE(wfile->Flush().ok());

  // Test sync error on file and dir
  f_env.SetFailSyncs(true);
  EXPECT_TRUE(wfile->Sync().IsIOError());
  EXPECT_TRUE(f_env.SyncDir(test_dir_).IsIOError());
  f_env.SetFailSyncs(false);
  ASSERT_TRUE(wfile->Sync().ok());
  ASSERT_TRUE(f_env.SyncDir(test_dir_).ok());

  // Test inactive filesystem
  f_env.SetFilesystemActive(false);
  EXPECT_TRUE(wfile->Append("more").IsIOError());
  EXPECT_TRUE(wfile->Flush().IsIOError());
  EXPECT_TRUE(wfile->Sync().IsIOError());
  EXPECT_TRUE(f_env.SyncDir(test_dir_).IsIOError());
  EXPECT_TRUE(f_env.RemoveFile(file_path).IsIOError());
  EXPECT_TRUE(f_env.RenameFile(file_path, Path("renamed.txt")).IsIOError());
  EXPECT_TRUE(f_env.CreateDir(Path("sub_dir")).IsIOError());
  EXPECT_TRUE(f_env.RemoveDir(Path("sub_dir")).IsIOError());

  std::unique_ptr<SequentialFile> seq_file;
  EXPECT_TRUE(f_env.NewSequentialFile(file_path, &seq_file).IsIOError());
  std::unique_ptr<RandomAccessFile> rnd_file;
  EXPECT_TRUE(f_env.NewRandomAccessFile(file_path, &rnd_file).IsIOError());

  f_env.SetFilesystemActive(true);

  ASSERT_TRUE(wfile->Close().ok());
}

}  // namespace
}  // namespace strata

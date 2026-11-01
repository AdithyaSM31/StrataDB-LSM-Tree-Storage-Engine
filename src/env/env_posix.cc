#include "env/env_posix.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "strata/env.h"
#include "strata/status.h"

namespace strata {

Status PosixError(std::string_view filename, int err) {
  std::string msg = std::system_category().message(err);
  if (err == ENOENT) {
    return Status::NotFound(filename, msg);
  }
  return Status::IOError(filename, msg);
}

Status PosixError(std::string_view filename, const std::error_code& ec) {
  if (ec.value() == ENOENT || ec == std::errc::no_such_file_or_directory) {
    return Status::NotFound(filename, ec.message());
  }
  return Status::IOError(filename, ec.message());
}

namespace {

class PosixSequentialFile : public SequentialFile {
 public:
  PosixSequentialFile(std::string filename, int fd) : filename_(std::move(filename)), fd_(fd) {}

  ~PosixSequentialFile() override {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  PosixSequentialFile(const PosixSequentialFile&) = delete;
  PosixSequentialFile& operator=(const PosixSequentialFile&) = delete;

  Status Read(size_t n, std::string_view* result, char* scratch) override {
    if (result == nullptr) {
      return Status::InvalidArgument("result pointer is null");
    }
    size_t total_read = 0;
    while (total_read < n) {
      ssize_t r = ::read(fd_, scratch + total_read, n - total_read);
      if (r < 0) {
        if (errno == EINTR) {
          continue;
        }
        return PosixError(filename_, errno);
      }
      if (r == 0) {
        // EOF reached.
        break;
      }
      total_read += static_cast<size_t>(r);
    }
    *result = std::string_view(scratch, total_read);
    return Status::OK();
  }

  Status Skip(uint64_t n) override {
    if (n > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
      return Status::InvalidArgument(filename_, "skip offset exceeds off_t max");
    }
    if (::lseek(fd_, static_cast<off_t>(n), SEEK_CUR) == static_cast<off_t>(-1)) {
      return PosixError(filename_, errno);
    }
    return Status::OK();
  }

 private:
  std::string filename_;
  int fd_;
};

class PosixRandomAccessFile : public RandomAccessFile {
 public:
  PosixRandomAccessFile(std::string filename, int fd) : filename_(std::move(filename)), fd_(fd) {}

  ~PosixRandomAccessFile() override {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  PosixRandomAccessFile(const PosixRandomAccessFile&) = delete;
  PosixRandomAccessFile& operator=(const PosixRandomAccessFile&) = delete;

  Status Read(uint64_t offset, size_t n, std::string_view* result, char* scratch) const override {
    if (result == nullptr) {
      return Status::InvalidArgument("result pointer is null");
    }
    if (offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
      return Status::InvalidArgument(filename_, "offset exceeds off_t max");
    }
    if (offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) - n) {
      return Status::InvalidArgument(filename_, "offset + size exceeds off_t max");
    }

    size_t total_read = 0;
    while (total_read < n) {
      ssize_t r = ::pread(fd_, scratch + total_read, n - total_read,
                          static_cast<off_t>(offset + total_read));
      if (r < 0) {
        if (errno == EINTR) {
          continue;
        }
        return PosixError(filename_, errno);
      }
      if (r == 0) {
        // Reached end of file; short read is expected, not an error.
        break;
      }
      total_read += static_cast<size_t>(r);
    }
    *result = std::string_view(scratch, total_read);
    return Status::OK();
  }

 private:
  std::string filename_;
  int fd_;
};

class PosixWritableFile : public WritableFile {
 public:
  static constexpr size_t kBufferSize = 65536;  // 64 KB

  PosixWritableFile(std::string filename, int fd)
      : filename_(std::move(filename)),
        fd_(fd),
        buf_(std::make_unique<char[]>(kBufferSize)),
        buf_pos_(0),
        closed_(false),
        has_error_(false) {}

  ~PosixWritableFile() override {
    if (!closed_ && fd_ >= 0) {
      if (!has_error_) {
        (void)FlushBuffer();
      }
      ::close(fd_);
    }
  }

  PosixWritableFile(const PosixWritableFile&) = delete;
  PosixWritableFile& operator=(const PosixWritableFile&) = delete;

  Status Append(std::string_view data) override {
    if (closed_) {
      return Status::IOError(filename_, "file already closed");
    }
    if (has_error_) {
      return Status::IOError(filename_, "file has previous error");
    }

    // 1. Fits in remaining buffer space.
    if (data.size() <= kBufferSize - buf_pos_) {
      std::memcpy(buf_.get() + buf_pos_, data.data(), data.size());
      buf_pos_ += data.size();
      return Status::OK();
    }

    // 2. Buffer has existing bytes that cannot hold all of data; flush them.
    if (buf_pos_ > 0) {
      Status s = FlushBuffer();
      if (!s.ok()) {
        return s;
      }
    }

    // Buffer is now empty.
    // 3. Writes larger than the buffer bypass it entirely.
    if (data.size() >= kBufferSize) {
      return WriteUnbuffered(data.data(), data.size());
    }

    // 4. Smaller write into empty buffer.
    std::memcpy(buf_.get(), data.data(), data.size());
    buf_pos_ = data.size();
    return Status::OK();
  }

  Status Flush() override {
    if (closed_) {
      return Status::IOError(filename_, "file already closed");
    }
    if (has_error_) {
      return Status::IOError(filename_, "file has previous error");
    }
    return FlushBuffer();
  }

  Status Sync() override {
    if (closed_) {
      return Status::IOError(filename_, "file already closed");
    }
    if (has_error_) {
      return Status::IOError(filename_, "file has previous error (fsyncgate)");
    }
    Status s = FlushBuffer();
    if (!s.ok()) {
      return s;
    }
#if defined(__APPLE__)
    if (::fcntl(fd_, F_FULLFSYNC) != 0) {
      int err = errno;
      has_error_ = true;
      return PosixError(filename_, err);
    }
#else
    if (::fdatasync(fd_) != 0) {
      int err = errno;
      has_error_ = true;
      return PosixError(filename_, err);
    }
#endif
    return Status::OK();
  }

  Status Close() override {
    if (closed_) {
      return Status::OK();
    }
    Status s = Status::OK();
    if (!has_error_) {
      s = FlushBuffer();
    }
    if (::close(fd_) != 0) {
      int err = errno;
      if (s.ok()) {
        s = PosixError(filename_, err);
      }
    }
    fd_ = -1;
    closed_ = true;
    return s;
  }

 private:
  Status WriteUnbuffered(const char* data, size_t size) {
    while (size > 0) {
      ssize_t r = ::write(fd_, data, size);
      if (r < 0) {
        if (errno == EINTR) {
          continue;
        }
        int err = errno;
        has_error_ = true;
        return PosixError(filename_, err);
      }
      data += r;
      size -= static_cast<size_t>(r);
    }
    return Status::OK();
  }

  Status FlushBuffer() {
    if (buf_pos_ == 0) {
      return Status::OK();
    }
    Status s = WriteUnbuffered(buf_.get(), buf_pos_);
    if (!s.ok()) {
      has_error_ = true;
      return s;
    }
    buf_pos_ = 0;
    return Status::OK();
  }

  std::string filename_;
  int fd_;
  std::unique_ptr<char[]> buf_;
  size_t buf_pos_;
  bool closed_;
  bool has_error_;
};

class PosixFileLock : public FileLock {
 public:
  PosixFileLock(int fd, std::string fname) : fd_(fd), fname_(std::move(fname)) {}

  ~PosixFileLock() override { Unlock(); }

  PosixFileLock(const PosixFileLock&) = delete;
  PosixFileLock& operator=(const PosixFileLock&) = delete;

  void Unlock() {
    if (fd_ >= 0) {
      ::flock(fd_, LOCK_UN);
      ::close(fd_);
      fd_ = -1;
    }
  }

  int fd() const { return fd_; }
  const std::string& fname() const { return fname_; }

 private:
  int fd_;
  std::string fname_;
};

}  // namespace

Status PosixEnv::NewSequentialFile(const std::string& fname,
                                   std::unique_ptr<SequentialFile>* result) {
  if (result == nullptr) {
    return Status::InvalidArgument("result pointer is null");
  }
  result->reset();
  int fd = ::open(fname.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return PosixError(fname, errno);
  }
  *result = std::make_unique<PosixSequentialFile>(fname, fd);
  return Status::OK();
}

Status PosixEnv::NewRandomAccessFile(const std::string& fname,
                                     std::unique_ptr<RandomAccessFile>* result) {
  if (result == nullptr) {
    return Status::InvalidArgument("result pointer is null");
  }
  result->reset();
  int fd = ::open(fname.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return PosixError(fname, errno);
  }
  *result = std::make_unique<PosixRandomAccessFile>(fname, fd);
  return Status::OK();
}

Status PosixEnv::NewWritableFile(const std::string& fname, std::unique_ptr<WritableFile>* result) {
  if (result == nullptr) {
    return Status::InvalidArgument("result pointer is null");
  }
  result->reset();
  int fd = ::open(fname.c_str(), O_TRUNC | O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) {
    return PosixError(fname, errno);
  }
  *result = std::make_unique<PosixWritableFile>(fname, fd);
  return Status::OK();
}

bool PosixEnv::FileExists(const std::string& fname) {
  std::error_code ec;
  return std::filesystem::exists(fname, ec);
}

Status PosixEnv::GetChildren(const std::string& dir, std::vector<std::string>* result) {
  if (result == nullptr) {
    return Status::InvalidArgument("result pointer is null");
  }
  result->clear();
  std::error_code ec;
  std::filesystem::directory_iterator it(dir, ec);
  if (ec) {
    return PosixError(dir, ec);
  }
  std::filesystem::directory_iterator end;
  while (it != end && !ec) {
    result->push_back(it->path().filename().string());
    it.increment(ec);
  }
  if (ec) {
    return PosixError(dir, ec);
  }
  return Status::OK();
}

Status PosixEnv::RemoveFile(const std::string& fname) {
  std::error_code ec;
  if (std::filesystem::is_directory(fname, ec)) {
    return Status::InvalidArgument(fname, "is a directory");
  }
  bool removed = std::filesystem::remove(fname, ec);
  if (ec) {
    return PosixError(fname, ec);
  }
  if (!removed) {
    return Status::NotFound(fname, "file does not exist");
  }
  return Status::OK();
}

Status PosixEnv::CreateDir(const std::string& dir) {
  std::error_code ec;
  if (!std::filesystem::create_directory(dir, ec)) {
    if (ec) {
      return PosixError(dir, ec);
    }
    return Status::IOError(dir, "directory already exists");
  }
  return Status::OK();
}

Status PosixEnv::RemoveDir(const std::string& dir) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec) && !ec) {
    return Status::InvalidArgument(dir, "not a directory");
  }
  bool removed = std::filesystem::remove(dir, ec);
  if (ec) {
    return PosixError(dir, ec);
  }
  if (!removed) {
    return Status::NotFound(dir, "directory does not exist");
  }
  return Status::OK();
}

Status PosixEnv::GetFileSize(const std::string& fname, uint64_t* size) {
  if (size == nullptr) {
    return Status::InvalidArgument("size pointer is null");
  }
  std::error_code ec;
  uintmax_t s = std::filesystem::file_size(fname, ec);
  if (ec) {
    return PosixError(fname, ec);
  }
  *size = static_cast<uint64_t>(s);
  return Status::OK();
}

Status PosixEnv::RenameFile(const std::string& src, const std::string& target) {
  if (::rename(src.c_str(), target.c_str()) != 0) {
    return PosixError(src, errno);
  }
  return Status::OK();
}

Status PosixEnv::SyncDir(const std::string& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return PosixError(dir, errno);
  }
  int res = ::fsync(fd);
  int err = errno;
  ::close(fd);
  if (res != 0) {
    return PosixError(dir, err);
  }
  return Status::OK();
}

Status PosixEnv::LockFile(const std::string& fname, std::unique_ptr<FileLock>* lock) {
  if (lock == nullptr) {
    return Status::InvalidArgument("lock pointer is null");
  }
  lock->reset();
  int fd = ::open(fname.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) {
    return PosixError(fname, errno);
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    int err = errno;
    ::close(fd);
    if (err == EWOULDBLOCK || err == EAGAIN) {
      return Status::IOError(fname, "lock held by another process");
    }
    return PosixError(fname, err);
  }
  *lock = std::make_unique<PosixFileLock>(fd, fname);
  return Status::OK();
}

Status PosixEnv::UnlockFile(std::unique_ptr<FileLock> lock) {
  if (!lock) {
    return Status::InvalidArgument("lock is null");
  }
  lock.reset();
  return Status::OK();
}

uint64_t PosixEnv::NowMicros() {
  auto now = std::chrono::steady_clock::now();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
}

void PosixEnv::SleepForMicroseconds(int micros) {
  if (micros > 0) {
    std::this_thread::sleep_for(std::chrono::microseconds(micros));
  }
}

}  // namespace strata

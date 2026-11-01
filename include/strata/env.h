#ifndef STRATA_INCLUDE_ENV_H_
#define STRATA_INCLUDE_ENV_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "strata/status.h"

namespace strata {

// A file abstraction for sequential reading (e.g. WAL and MANIFEST recovery).
class SequentialFile {
 public:
  virtual ~SequentialFile() = default;

  // Reads up to n bytes from the file.
  //
  // scratch must be at least n bytes long.
  // result is set to the data read.
  // The contract is that *result may point into scratch, so it is only valid
  // until scratch is reused or modified.
  //
  // Returns OK on success or EOF. When EOF is reached before n bytes are read,
  // *result will contain fewer than n bytes (empty if already at EOF).
  virtual Status Read(size_t n, std::string_view* result, char* scratch) = 0;

  // Skips n bytes from the current file position.
  virtual Status Skip(uint64_t n) = 0;
};

// A file abstraction for random access reading (e.g. SSTable reads).
// All methods must be const and thread-safe.
class RandomAccessFile {
 public:
  virtual ~RandomAccessFile() = default;

  // Reads up to n bytes from the file starting at offset.
  //
  // scratch must be at least n bytes long.
  // result is set to the data read.
  // The contract is that *result may point into scratch, so it is only valid
  // until scratch is reused or modified.
  //
  // const and thread-safe: multiple threads may call Read concurrently
  // with arbitrary offsets.
  //
  // Reading past the end returns a short or empty result, not an error.
  virtual Status Read(uint64_t offset, size_t n, std::string_view* result, char* scratch) const = 0;
};

// A file abstraction for sequential writing (e.g. WAL writer, SSTable builder).
class WritableFile {
 public:
  virtual ~WritableFile() = default;

  // Appends data to the file, buffering small writes in an internal buffer.
  virtual Status Append(std::string_view data) = 0;

  // Flushes any buffered bytes to the OS page cache (not durable).
  virtual Status Flush() = 0;

  // Flushes buffered bytes and makes all written data durable (fdatasync).
  // If Sync fails, subsequent calls must not retry fsync/fdatasync (fsyncgate).
  virtual Status Sync() = 0;

  // Flushes any buffered data and closes the file.
  virtual Status Close() = 0;
};

// An opaque handle to an acquired file lock.
class FileLock {
 public:
  virtual ~FileLock() = default;
};

// The abstract interface to operating system services. Every file read, write,
// rename, sync, and lock in the engine goes through this interface.
class Env {
 public:
  virtual ~Env() = default;

  // Returns the default POSIX environment. The returned pointer is deliberately
  // never destroyed and remains valid for the lifetime of the process.
  static Env* Default();

  // Creates a brand new sequential file reader for fname.
  virtual Status NewSequentialFile(const std::string& fname,
                                   std::unique_ptr<SequentialFile>* result) = 0;

  // Creates a brand new random access file reader for fname.
  virtual Status NewRandomAccessFile(const std::string& fname,
                                     std::unique_ptr<RandomAccessFile>* result) = 0;

  // Creates an object that writes to a new file with the specified name.
  // Truncates any existing file with the same name.
  virtual Status NewWritableFile(const std::string& fname,
                                 std::unique_ptr<WritableFile>* result) = 0;

  // Returns true iff the named file or directory exists.
  virtual bool FileExists(const std::string& fname) = 0;

  // Stores in *result the names of the children of the specified directory.
  // The entries '.' and '..' are excluded.
  virtual Status GetChildren(const std::string& dir, std::vector<std::string>* result) = 0;

  // Deletes the named file.
  virtual Status RemoveFile(const std::string& fname) = 0;

  // Creates the specified directory.
  virtual Status CreateDir(const std::string& dir) = 0;

  // Deletes the specified directory.
  virtual Status RemoveDir(const std::string& dir) = 0;

  // Stores the size of fname in *size.
  virtual Status GetFileSize(const std::string& fname, uint64_t* size) = 0;

  // Atomically renames src to target. If target already exists, it is overwritten.
  virtual Status RenameFile(const std::string& src, const std::string& target) = 0;

  // Synchronizes the directory itself to durable storage (fsync on directory fd).
  virtual Status SyncDir(const std::string& dir) = 0;

  // Acquires an exclusive lock on fname using flock.
  // Returns an IOError if the lock is held by another process or file description.
  virtual Status LockFile(const std::string& fname, std::unique_ptr<FileLock>* lock) = 0;

  // Releases a previously acquired lock without deleting the lock file.
  virtual Status UnlockFile(std::unique_ptr<FileLock> lock) = 0;

  // Returns the number of microseconds since an arbitrary epoch using steady_clock.
  virtual uint64_t NowMicros() = 0;

  // Sleeps for the specified number of microseconds.
  virtual void SleepForMicroseconds(int micros) = 0;
};

// An Env wrapper that forwards every call to target. FaultInjectionEnv and
// counting Env can override only the specific methods they care about.
class EnvWrapper : public Env {
 public:
  explicit EnvWrapper(Env* target) : target_(target) {}
  ~EnvWrapper() override = default;

  Env* target() const { return target_; }

  Status NewSequentialFile(const std::string& fname,
                           std::unique_ptr<SequentialFile>* result) override {
    return target_->NewSequentialFile(fname, result);
  }

  Status NewRandomAccessFile(const std::string& fname,
                             std::unique_ptr<RandomAccessFile>* result) override {
    return target_->NewRandomAccessFile(fname, result);
  }

  Status NewWritableFile(const std::string& fname, std::unique_ptr<WritableFile>* result) override {
    return target_->NewWritableFile(fname, result);
  }

  bool FileExists(const std::string& fname) override { return target_->FileExists(fname); }

  Status GetChildren(const std::string& dir, std::vector<std::string>* result) override {
    return target_->GetChildren(dir, result);
  }

  Status RemoveFile(const std::string& fname) override { return target_->RemoveFile(fname); }

  Status CreateDir(const std::string& dir) override { return target_->CreateDir(dir); }

  Status RemoveDir(const std::string& dir) override { return target_->RemoveDir(dir); }

  Status GetFileSize(const std::string& fname, uint64_t* size) override {
    return target_->GetFileSize(fname, size);
  }

  Status RenameFile(const std::string& src, const std::string& target) override {
    return target_->RenameFile(src, target);
  }

  Status SyncDir(const std::string& dir) override { return target_->SyncDir(dir); }

  Status LockFile(const std::string& fname, std::unique_ptr<FileLock>* lock) override {
    return target_->LockFile(fname, lock);
  }

  Status UnlockFile(std::unique_ptr<FileLock> lock) override {
    return target_->UnlockFile(std::move(lock));
  }

  uint64_t NowMicros() override { return target_->NowMicros(); }

  void SleepForMicroseconds(int micros) override { target_->SleepForMicroseconds(micros); }

 private:
  Env* target_;
};

// Wrapper classes forwarding to underlying file implementations.
class SequentialFileWrapper : public SequentialFile {
 public:
  explicit SequentialFileWrapper(SequentialFile* target) : target_(target) {}
  ~SequentialFileWrapper() override = default;

  Status Read(size_t n, std::string_view* result, char* scratch) override {
    return target_->Read(n, result, scratch);
  }
  Status Skip(uint64_t n) override { return target_->Skip(n); }

 private:
  SequentialFile* target_;
};

class RandomAccessFileWrapper : public RandomAccessFile {
 public:
  explicit RandomAccessFileWrapper(RandomAccessFile* target) : target_(target) {}
  ~RandomAccessFileWrapper() override = default;

  Status Read(uint64_t offset, size_t n, std::string_view* result, char* scratch) const override {
    return target_->Read(offset, n, result, scratch);
  }

 private:
  RandomAccessFile* target_;
};

class WritableFileWrapper : public WritableFile {
 public:
  explicit WritableFileWrapper(WritableFile* target) : target_(target) {}
  ~WritableFileWrapper() override = default;

  Status Append(std::string_view data) override { return target_->Append(data); }
  Status Flush() override { return target_->Flush(); }
  Status Sync() override { return target_->Sync(); }
  Status Close() override { return target_->Close(); }

 private:
  WritableFile* target_;
};

}  // namespace strata

#endif  // STRATA_INCLUDE_ENV_H_

#ifndef STRATA_ENV_ENV_POSIX_H_
#define STRATA_ENV_ENV_POSIX_H_

#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "strata/env.h"
#include "strata/status.h"

namespace strata {

// Helper functions that convert errno or std::error_code to Status.
// ENOENT maps to Status::NotFound; other errors map to Status::IOError.
Status PosixError(std::string_view filename, int err);
Status PosixError(std::string_view filename, const std::error_code& ec);

class PosixEnv : public Env {
 public:
  PosixEnv() = default;
  ~PosixEnv() override = default;

  Status NewSequentialFile(const std::string& fname,
                           std::unique_ptr<SequentialFile>* result) override;
  Status NewRandomAccessFile(const std::string& fname,
                             std::unique_ptr<RandomAccessFile>* result) override;
  Status NewWritableFile(const std::string& fname, std::unique_ptr<WritableFile>* result) override;

  bool FileExists(const std::string& fname) override;
  Status GetChildren(const std::string& dir, std::vector<std::string>* result) override;
  Status RemoveFile(const std::string& fname) override;
  Status CreateDir(const std::string& dir) override;
  Status RemoveDir(const std::string& dir) override;
  Status GetFileSize(const std::string& fname, uint64_t* size) override;
  Status RenameFile(const std::string& src, const std::string& target) override;
  Status SyncDir(const std::string& dir) override;

  Status LockFile(const std::string& fname, std::unique_ptr<FileLock>* lock) override;
  Status UnlockFile(std::unique_ptr<FileLock> lock) override;

  uint64_t NowMicros() override;
  void SleepForMicroseconds(int micros) override;
};

}  // namespace strata

#endif  // STRATA_ENV_ENV_POSIX_H_

#ifndef STRATA_UTIL_FAULT_INJECTION_ENV_H_
#define STRATA_UTIL_FAULT_INJECTION_ENV_H_

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "strata/env.h"
#include "strata/status.h"

namespace strata {

class FaultInjectionEnv;

class FaultInjectionWritableFile : public WritableFile {
 public:
  struct FileState {
    std::string fname;
    std::atomic<uint64_t> pos{0};
    std::atomic<uint64_t> synced_pos{0};

    explicit FileState(std::string name) : fname(std::move(name)) {}
  };

  FaultInjectionWritableFile(std::string fname, std::shared_ptr<FileState> state,
                             std::unique_ptr<WritableFile> target, FaultInjectionEnv* env);

  ~FaultInjectionWritableFile() override;

  Status Append(std::string_view data) override;
  Status Flush() override;
  Status Sync() override;
  Status Close() override;

  void CrashClose();

 private:
  const std::string fname_;
  std::shared_ptr<FileState> state_;
  std::unique_ptr<WritableFile> target_;
  FaultInjectionEnv* const env_;
  std::mutex mutex_;
  bool crashed_{false};
};

class FaultInjectionEnv : public EnvWrapper {
 public:
  explicit FaultInjectionEnv(Env* target) : EnvWrapper(target) {}
  ~FaultInjectionEnv() override = default;

  Status NewWritableFile(const std::string& fname, std::unique_ptr<WritableFile>* result) override {
    if (!filesystem_active_.load(std::memory_order_acquire)) {
      return Status::IOError("injected fault: filesystem inactive");
    }
    if (fail_new_writable_file_.load(std::memory_order_acquire)) {
      return Status::IOError("injected fault: NewWritableFile failed");
    }

    std::unique_ptr<WritableFile> base_file;
    Status s = EnvWrapper::NewWritableFile(fname, &base_file);
    if (!s.ok()) {
      return s;
    }

    std::shared_ptr<FaultInjectionWritableFile::FileState> state;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = files_.find(fname);
      if (it == files_.end()) {
        state = std::make_shared<FaultInjectionWritableFile::FileState>(fname);
        files_[fname] = state;
      } else {
        state = it->second;
        state->pos.store(0, std::memory_order_relaxed);
        state->synced_pos.store(0, std::memory_order_relaxed);
      }
    }

    auto file =
        std::make_unique<FaultInjectionWritableFile>(fname, state, std::move(base_file), this);
    RegisterOpenFile(file.get());
    *result = std::move(file);
    return Status::OK();
  }

  Status NewSequentialFile(const std::string& fname,
                           std::unique_ptr<SequentialFile>* result) override {
    if (!filesystem_active_.load(std::memory_order_acquire)) {
      return Status::IOError("injected fault: filesystem inactive");
    }
    return EnvWrapper::NewSequentialFile(fname, result);
  }

  Status NewRandomAccessFile(const std::string& fname,
                             std::unique_ptr<RandomAccessFile>* result) override {
    if (!filesystem_active_.load(std::memory_order_acquire)) {
      return Status::IOError("injected fault: filesystem inactive");
    }
    return EnvWrapper::NewRandomAccessFile(fname, result);
  }

  Status RemoveFile(const std::string& fname) override {
    if (!filesystem_active_.load(std::memory_order_acquire)) {
      return Status::IOError("injected fault: filesystem inactive");
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      files_.erase(fname);
    }
    return EnvWrapper::RemoveFile(fname);
  }

  Status RenameFile(const std::string& src, const std::string& target) override {
    if (!filesystem_active_.load(std::memory_order_acquire)) {
      return Status::IOError("injected fault: filesystem inactive");
    }
    Status s = EnvWrapper::RenameFile(src, target);
    if (s.ok()) {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = files_.find(src);
      if (it != files_.end()) {
        auto state = it->second;
        files_.erase(it);
        state->fname = target;
        files_[target] = state;
      }
    }
    return s;
  }

  Status SyncDir(const std::string& dir) override {
    if (!filesystem_active_.load(std::memory_order_acquire)) {
      return Status::IOError("injected fault: filesystem inactive");
    }
    if (fail_syncs_.load(std::memory_order_acquire)) {
      return Status::IOError("injected fault: sync failed");
    }
    return EnvWrapper::SyncDir(dir);
  }

  Status CreateDir(const std::string& dir) override {
    if (!filesystem_active_.load(std::memory_order_acquire)) {
      return Status::IOError("injected fault: filesystem inactive");
    }
    return EnvWrapper::CreateDir(dir);
  }

  Status RemoveDir(const std::string& dir) override {
    if (!filesystem_active_.load(std::memory_order_acquire)) {
      return Status::IOError("injected fault: filesystem inactive");
    }
    return EnvWrapper::RemoveDir(dir);
  }

  void SetFilesystemActive(bool active) {
    filesystem_active_.store(active, std::memory_order_release);
  }

  bool IsFilesystemActive() const { return filesystem_active_.load(std::memory_order_acquire); }

  void SetFailWrites(bool fail) { fail_writes_.store(fail, std::memory_order_release); }

  bool FailWrites() const { return fail_writes_.load(std::memory_order_acquire); }

  void SetFailSyncs(bool fail) { fail_syncs_.store(fail, std::memory_order_release); }

  bool FailSyncs() const { return fail_syncs_.load(std::memory_order_acquire); }

  void SetFailNewWritableFile(bool fail) {
    fail_new_writable_file_.store(fail, std::memory_order_release);
  }

  // Crash simulation: close open writable files so unflushed userspace data is
  // discarded, and truncate every file written to the length it had at its last
  // successful Sync(). Unsynced writes are lost.
  Status SimulateCrash() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto* file : open_files_) {
      file->CrashClose();
    }
    for (const auto& [fname, state] : files_) {
      const uint64_t synced = state->synced_pos.load(std::memory_order_relaxed);
      const uint64_t current = state->pos.load(std::memory_order_relaxed);
      if (current > synced) {
        Status s = TruncateFile(fname, synced);
        if (!s.ok()) {
          return s;
        }
        state->pos.store(synced, std::memory_order_relaxed);
      }
    }
    return Status::OK();
  }

  void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    filesystem_active_.store(true, std::memory_order_release);
    fail_writes_.store(false, std::memory_order_release);
    fail_syncs_.store(false, std::memory_order_release);
    fail_new_writable_file_.store(false, std::memory_order_release);
    files_.clear();
  }

  void RegisterOpenFile(FaultInjectionWritableFile* f) {
    std::lock_guard<std::mutex> lock(mutex_);
    open_files_.insert(f);
  }

  void UnregisterOpenFile(FaultInjectionWritableFile* f) {
    std::lock_guard<std::mutex> lock(mutex_);
    open_files_.erase(f);
  }

 private:
  friend class FaultInjectionWritableFile;

  Status TruncateFile(const std::string& fname, uint64_t target_size) {
    if (!FileExists(fname)) {
      return Status::OK();
    }
    // Attempt POSIX truncate first.
    if (::truncate(fname.c_str(), static_cast<off_t>(target_size)) == 0) {
      return Status::OK();
    }

    // Fallback: copy target_size bytes using sequential read and writable write.
    std::unique_ptr<SequentialFile> rfile;
    Status s = EnvWrapper::NewSequentialFile(fname, &rfile);
    if (!s.ok()) {
      return s;
    }

    std::string content;
    content.resize(target_size);
    size_t offset = 0;
    while (offset < target_size) {
      char buf[8192];
      size_t chunk = std::min(sizeof(buf), static_cast<size_t>(target_size - offset));
      std::string_view piece;
      s = rfile->Read(chunk, &piece, buf);
      if (!s.ok()) {
        return s;
      }
      if (piece.empty()) {
        break;
      }
      content.replace(offset, piece.size(), piece.data(), piece.size());
      offset += piece.size();
    }
    content.resize(offset);
    rfile.reset();

    std::unique_ptr<WritableFile> wfile;
    s = EnvWrapper::NewWritableFile(fname, &wfile);
    if (!s.ok()) {
      return s;
    }
    s = wfile->Append(content);
    if (!s.ok()) {
      return s;
    }
    s = wfile->Sync();
    if (!s.ok()) {
      return s;
    }
    return wfile->Close();
  }

  std::mutex mutex_;
  std::atomic<bool> filesystem_active_{true};
  std::atomic<bool> fail_writes_{false};
  std::atomic<bool> fail_syncs_{false};
  std::atomic<bool> fail_new_writable_file_{false};
  std::map<std::string, std::shared_ptr<FaultInjectionWritableFile::FileState>> files_;
  std::set<FaultInjectionWritableFile*> open_files_;
};

inline FaultInjectionWritableFile::FaultInjectionWritableFile(std::string fname,
                                                              std::shared_ptr<FileState> state,
                                                              std::unique_ptr<WritableFile> target,
                                                              FaultInjectionEnv* env)
    : fname_(std::move(fname)), state_(std::move(state)), target_(std::move(target)), env_(env) {}

inline FaultInjectionWritableFile::~FaultInjectionWritableFile() { env_->UnregisterOpenFile(this); }

inline void FaultInjectionWritableFile::CrashClose() {
  std::lock_guard<std::mutex> lock(mutex_);
  crashed_ = true;
  if (target_ != nullptr) {
    (void)target_->Close();
    target_.reset();
  }
}

inline Status FaultInjectionWritableFile::Append(std::string_view data) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (crashed_ || target_ == nullptr) {
    return Status::IOError("injected fault: simulated crash");
  }
  if (!env_->IsFilesystemActive()) {
    return Status::IOError("injected fault: filesystem inactive");
  }
  if (env_->FailWrites()) {
    return Status::IOError("injected fault: write failed");
  }
  Status s = target_->Append(data);
  if (s.ok()) {
    state_->pos.fetch_add(data.size(), std::memory_order_relaxed);
  }
  return s;
}

inline Status FaultInjectionWritableFile::Flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (crashed_ || target_ == nullptr) {
    return Status::IOError("injected fault: simulated crash");
  }
  if (!env_->IsFilesystemActive()) {
    return Status::IOError("injected fault: filesystem inactive");
  }
  if (env_->FailWrites()) {
    return Status::IOError("injected fault: write failed");
  }
  return target_->Flush();
}

inline Status FaultInjectionWritableFile::Sync() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (crashed_ || target_ == nullptr) {
    return Status::IOError("injected fault: simulated crash");
  }
  if (!env_->IsFilesystemActive()) {
    return Status::IOError("injected fault: filesystem inactive");
  }
  if (env_->FailSyncs()) {
    return Status::IOError("injected fault: sync failed");
  }
  Status s = target_->Sync();
  if (s.ok()) {
    state_->synced_pos.store(state_->pos.load(std::memory_order_relaxed),
                             std::memory_order_release);
  }
  return s;
}

inline Status FaultInjectionWritableFile::Close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (crashed_ || target_ == nullptr) {
    return Status::OK();
  }
  Status s = target_->Close();
  target_.reset();
  return s;
}

}  // namespace strata

#endif  // STRATA_UTIL_FAULT_INJECTION_ENV_H_

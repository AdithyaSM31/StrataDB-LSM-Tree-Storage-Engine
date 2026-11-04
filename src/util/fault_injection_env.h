#ifndef STRATA_UTIL_FAULT_INJECTION_ENV_H_
#define STRATA_UTIL_FAULT_INJECTION_ENV_H_

#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <random>
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
  const bool is_manifest_;
  std::shared_ptr<FileState> state_;
  std::unique_ptr<WritableFile> target_;
  FaultInjectionEnv* const env_;
  std::mutex mutex_;
  bool crashed_{false};
};

// Simulates power loss on top of a real Env. What a crash loses:
//   - bytes appended to a file after its last successful Sync();
//   - files created, and renames made, since the last SyncDir() of their
//     directory: the directory entry itself was never durable.
// A removal is treated as durable at once, so a deleted file never comes back.
//
// A crash stops the filesystem: every later operation fails until
// ResumeAfterCrash(), so background threads of the dead DB cannot change what
// the next Open sees.
class FaultInjectionEnv : public EnvWrapper {
 public:
  // What happens to the bytes after a file's last Sync() in a crash.
  enum class UnsyncedData : uint8_t {
    kDropAll,         // all of them are lost
    kKeepRandomTail,  // a random prefix survives, usually ending mid-record
  };

  explicit FaultInjectionEnv(Env* target) : EnvWrapper(target) {}
  ~FaultInjectionEnv() override = default;

  Status NewWritableFile(const std::string& fname, std::unique_ptr<WritableFile>* result) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!BeginOperation()) {
      return Status::IOError("injected fault: filesystem inactive");
    }
    if (fail_new_writable_file_.load(std::memory_order_acquire)) {
      return Status::IOError("injected fault: NewWritableFile failed");
    }

    const bool existed = FileExists(fname);
    std::unique_ptr<WritableFile> base_file;
    Status s = EnvWrapper::NewWritableFile(fname, &base_file);
    if (!s.ok()) {
      return s;
    }
    if (!existed) {
      PendingDirOp op;
      op.is_rename = false;
      op.dir = DirName(fname);
      op.name = fname;
      pending_dir_ops_.push_back(std::move(op));
    }

    std::shared_ptr<FaultInjectionWritableFile::FileState> state;
    auto it = files_.find(fname);
    if (it == files_.end()) {
      state = std::make_shared<FaultInjectionWritableFile::FileState>(fname);
      files_[fname] = state;
    } else {
      state = it->second;
      state->pos.store(0, std::memory_order_relaxed);
      state->synced_pos.store(0, std::memory_order_relaxed);
    }

    auto file =
        std::make_unique<FaultInjectionWritableFile>(fname, state, std::move(base_file), this);
    open_files_.insert(file.get());
    *result = std::move(file);
    return Status::OK();
  }

  Status NewSequentialFile(const std::string& fname,
                           std::unique_ptr<SequentialFile>* result) override {
    if (!IsFilesystemActive()) {
      return Status::IOError("injected fault: filesystem inactive");
    }
    return EnvWrapper::NewSequentialFile(fname, result);
  }

  Status NewRandomAccessFile(const std::string& fname,
                             std::unique_ptr<RandomAccessFile>* result) override {
    if (!IsFilesystemActive()) {
      return Status::IOError("injected fault: filesystem inactive");
    }
    return EnvWrapper::NewRandomAccessFile(fname, result);
  }

  Status RemoveFile(const std::string& fname) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!BeginOperation()) {
      return Status::IOError("injected fault: filesystem inactive");
    }
    files_.erase(fname);
    return EnvWrapper::RemoveFile(fname);
  }

  Status RenameFile(const std::string& src, const std::string& target) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!BeginOperation()) {
      return Status::IOError("injected fault: filesystem inactive");
    }
    PendingDirOp op;
    op.is_rename = true;
    op.dir = DirName(target);
    op.src = src;
    op.name = target;
    op.target_existed = FileExists(target);
    if (op.target_existed) {
      Status s = ReadWholeFile(target, &op.old_target_contents);
      if (!s.ok()) {
        return s;
      }
    }
    Status s = EnvWrapper::RenameFile(src, target);
    if (s.ok()) {
      pending_dir_ops_.push_back(std::move(op));
      MoveFileState(src, target);
    }
    return s;
  }

  Status SyncDir(const std::string& dir) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!BeginOperation()) {
      return Status::IOError("injected fault: filesystem inactive");
    }
    if (fail_syncs_.load(std::memory_order_acquire)) {
      return Status::IOError("injected fault: sync failed");
    }
    Status s = EnvWrapper::SyncDir(dir);
    if (s.ok()) {
      std::vector<PendingDirOp> remaining;
      for (auto& op : pending_dir_ops_) {
        if (op.dir != dir) {
          remaining.push_back(std::move(op));
        }
      }
      pending_dir_ops_ = std::move(remaining);
    }
    return s;
  }

  Status CreateDir(const std::string& dir) override {
    if (!IsFilesystemActive()) {
      return Status::IOError("injected fault: filesystem inactive");
    }
    return EnvWrapper::CreateDir(dir);
  }

  Status RemoveDir(const std::string& dir) override {
    if (!IsFilesystemActive()) {
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

  // Sync() on a MANIFEST reports failure, while its bytes stay in the file. That
  // is what a failed fsync leaves behind: nobody knows whether the data is durable.
  void SetFailManifestSyncs(bool fail) {
    fail_manifest_syncs_.store(fail, std::memory_order_release);
  }

  bool FailManifestSyncs() const { return fail_manifest_syncs_.load(std::memory_order_acquire); }

  void SetFailNewWritableFile(bool fail) {
    fail_new_writable_file_.store(fail, std::memory_order_release);
  }

  // Stops the filesystem when the n-th operation that changes something starts
  // (file creation, removal, rename, Append, Sync, SyncDir), counted from now.
  // That operation and all later ones fail. Pass 0 to turn this off.
  void SetCrashAtOperation(uint64_t n) {
    operations_.store(0, std::memory_order_release);
    crash_at_operation_.store(n, std::memory_order_release);
  }

  // Operations counted since the last SetCrashAtOperation() call.
  uint64_t operations() const { return operations_.load(std::memory_order_acquire); }

  // Stops the filesystem and applies the crash to the files on disk. Call
  // ResumeAfterCrash() once the DB using this Env has been destroyed.
  Status SimulateCrash(UnsyncedData mode = UnsyncedData::kDropAll, uint32_t seed = 0) {
    SetFilesystemActive(false);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto* file : open_files_) {
      file->CrashClose();
    }

    std::mt19937 rnd(seed);
    for (const auto& [fname, state] : files_) {
      const uint64_t synced = state->synced_pos.load(std::memory_order_relaxed);
      const uint64_t current = state->pos.load(std::memory_order_relaxed);
      if (current > synced) {
        uint64_t keep = synced;
        if (mode == UnsyncedData::kKeepRandomTail) {
          keep += rnd() % (current - synced + 1);
        }
        Status s = TruncateFile(fname, keep);
        if (!s.ok()) {
          return s;
        }
        state->pos.store(keep, std::memory_order_relaxed);
        state->synced_pos.store(keep, std::memory_order_relaxed);
      }
    }

    // Newest first, so a file renamed after it was created is undone correctly.
    for (auto it = pending_dir_ops_.rbegin(); it != pending_dir_ops_.rend(); ++it) {
      Status s = it->is_rename ? UndoRename(*it) : UndoCreate(*it);
      if (!s.ok()) {
        return s;
      }
    }
    pending_dir_ops_.clear();
    return Status::OK();
  }

  // Starts the filesystem again after a crash, with the injected failures off.
  void ResumeAfterCrash() {
    crash_at_operation_.store(0, std::memory_order_release);
    fail_manifest_syncs_.store(false, std::memory_order_release);
    SetFilesystemActive(true);
  }

  void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    filesystem_active_.store(true, std::memory_order_release);
    fail_writes_.store(false, std::memory_order_release);
    fail_syncs_.store(false, std::memory_order_release);
    fail_manifest_syncs_.store(false, std::memory_order_release);
    fail_new_writable_file_.store(false, std::memory_order_release);
    crash_at_operation_.store(0, std::memory_order_release);
    operations_.store(0, std::memory_order_release);
    files_.clear();
    pending_dir_ops_.clear();
  }

  void UnregisterOpenFile(FaultInjectionWritableFile* f) {
    std::lock_guard<std::mutex> lock(mutex_);
    open_files_.erase(f);
  }

 private:
  friend class FaultInjectionWritableFile;

  // A directory change that a crash undoes unless SyncDir() has run since.
  struct PendingDirOp {
    bool is_rename = false;
    std::string dir;
    std::string name;  // the created file, or the rename target
    std::string src;   // rename only
    bool target_existed = false;
    std::string old_target_contents;
  };

  static std::string DirName(const std::string& fname) {
    const auto slash = fname.rfind('/');
    return slash == std::string::npos ? std::string(".") : fname.substr(0, slash);
  }

  // Counts an operation. Returns false if the filesystem is stopped, including
  // when this operation is the one chosen to stop it.
  bool BeginOperation() {
    if (!IsFilesystemActive()) {
      return false;
    }
    const uint64_t n = operations_.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (n == crash_at_operation_.load(std::memory_order_acquire)) {
      SetFilesystemActive(false);
      return false;
    }
    return true;
  }

  void MoveFileState(const std::string& from, const std::string& to) {
    files_.erase(to);
    auto it = files_.find(from);
    if (it != files_.end()) {
      auto state = it->second;
      files_.erase(it);
      state->fname = to;
      files_[to] = state;
    }
  }

  Status ReadWholeFile(const std::string& fname, std::string* contents) {
    contents->clear();
    std::unique_ptr<SequentialFile> file;
    Status s = EnvWrapper::NewSequentialFile(fname, &file);
    if (!s.ok()) {
      return s;
    }
    char buf[8192];
    while (true) {
      std::string_view piece;
      s = file->Read(sizeof(buf), &piece, buf);
      if (!s.ok() || piece.empty()) {
        return s;
      }
      contents->append(piece);
    }
  }

  Status UndoRename(const PendingDirOp& op) {
    if (FileExists(op.name)) {
      Status s = EnvWrapper::RenameFile(op.name, op.src);
      if (!s.ok()) {
        return s;
      }
      MoveFileState(op.name, op.src);
    }
    if (!op.target_existed) {
      return Status::OK();
    }
    std::unique_ptr<WritableFile> file;
    Status s = EnvWrapper::NewWritableFile(op.name, &file);
    if (s.ok()) {
      s = file->Append(op.old_target_contents);
    }
    if (s.ok()) {
      s = file->Sync();
    }
    if (s.ok()) {
      s = file->Close();
    }
    return s;
  }

  Status UndoCreate(const PendingDirOp& op) {
    files_.erase(op.name);
    return FileExists(op.name) ? EnvWrapper::RemoveFile(op.name) : Status::OK();
  }

  Status TruncateFile(const std::string& fname, uint64_t target_size) {
    if (::truncate(fname.c_str(), static_cast<off_t>(target_size)) != 0 && errno != ENOENT) {
      return Status::IOError(fname, "truncate failed");
    }
    return Status::OK();
  }

  std::mutex mutex_;  // held across directory operations, so a crash sees each done or not begun
  std::atomic<bool> filesystem_active_{true};
  std::atomic<bool> fail_writes_{false};
  std::atomic<bool> fail_syncs_{false};
  std::atomic<bool> fail_manifest_syncs_{false};
  std::atomic<bool> fail_new_writable_file_{false};
  std::atomic<uint64_t> crash_at_operation_{0};
  std::atomic<uint64_t> operations_{0};
  std::map<std::string, std::shared_ptr<FaultInjectionWritableFile::FileState>> files_;
  std::set<FaultInjectionWritableFile*> open_files_;
  std::vector<PendingDirOp> pending_dir_ops_;
};

inline FaultInjectionWritableFile::FaultInjectionWritableFile(std::string fname,
                                                              std::shared_ptr<FileState> state,
                                                              std::unique_ptr<WritableFile> target,
                                                              FaultInjectionEnv* env)
    : fname_(std::move(fname)),
      is_manifest_(fname_.find("/MANIFEST-") != std::string::npos),
      state_(std::move(state)),
      target_(std::move(target)),
      env_(env) {}

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
  if (!env_->BeginOperation()) {
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
  if (!env_->BeginOperation()) {
    return Status::IOError("injected fault: filesystem inactive");
  }
  if (env_->FailSyncs()) {
    return Status::IOError("injected fault: sync failed");
  }
  if (is_manifest_ && env_->FailManifestSyncs()) {
    (void)target_->Flush();
    return Status::IOError("injected fault: MANIFEST sync failed");
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

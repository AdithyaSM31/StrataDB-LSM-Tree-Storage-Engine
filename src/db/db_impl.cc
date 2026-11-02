#include "db/db_impl.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "db/dbformat.h"
#include "db/filename.h"
#include "db/version_edit.h"
#include "db/version_set.h"
#include "db/write_batch_internal.h"
#include "log/log_reader.h"
#include "log/log_writer.h"
#include "memtable/memtable.h"
#include "strata/db.h"
#include "strata/env.h"
#include "strata/options.h"
#include "strata/status.h"
#include "util/check.h"

namespace strata {

Status DB::Open(const Options& options, const std::string& name, std::unique_ptr<DB>* out) {
  if (out == nullptr) {
    return Status::InvalidArgument("out pointer is null");
  }
  out->reset();

  auto impl = std::make_unique<DBImpl>(options, name);
  Status s = impl->Recover();
  if (s.ok()) {
    *out = std::move(impl);
  }
  return s;
}

DBImpl::DBImpl(const Options& options, const std::string& dbname)
    : env_(options.env == nullptr ? Env::Default() : options.env),
      options_(options),
      dbname_(dbname),
      icmp_(),
      snapshots_() {}

DBImpl::~DBImpl() {
  if (log_ != nullptr) {
    log_.reset();
  }
  if (logfile_ != nullptr) {
    (void)logfile_->Close();
    logfile_.reset();
  }
  if (mem_ != nullptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    mem_->Unref();
    mem_ = nullptr;
  }
  if (versions_ != nullptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    versions_.reset();
  }
  if (db_lock_ != nullptr) {
    (void)env_->UnlockFile(std::move(db_lock_));
  }
}

Status DBImpl::NewDB() {
  VersionEdit new_db;
  new_db.SetComparatorName(icmp_.Name());
  new_db.SetLogNumber(0);
  new_db.SetNextFile(2);
  new_db.SetLastSequence(0);

  const std::string manifest = DescriptorFileName(dbname_, 1);
  std::unique_ptr<WritableFile> file;
  Status s = env_->NewWritableFile(manifest, &file);
  if (!s.ok()) {
    return s;
  }
  {
    log::Writer log(file.get());
    std::string record;
    new_db.EncodeTo(&record);
    s = log.AddRecord(record);
    if (s.ok()) {
      s = file->Sync();
    }
  }
  if (s.ok()) {
    s = file->Close();
  }
  if (s.ok()) {
    s = SetCurrentFile(env_, dbname_, 1);
  } else {
    (void)env_->RemoveFile(manifest);
  }
  return s;
}

Status DBImpl::Recover() {
  const bool dir_exists = env_->FileExists(dbname_);
  if (!dir_exists) {
    if (!options_.create_if_missing) {
      return Status::InvalidArgument(dbname_, "does not exist (create_if_missing is false)");
    }
    Status s = env_->CreateDir(dbname_);
    if (!s.ok()) {
      return s;
    }
  }

  // Acquire exclusive file lock on LockFileName(dbname_)
  Status s = env_->LockFile(LockFileName(dbname_), &db_lock_);
  if (!s.ok()) {
    return s;
  }

  const bool current_exists = env_->FileExists(CurrentFileName(dbname_));

  // Find all children for legacy logs scan / existence check
  std::vector<std::string> filenames;
  s = env_->GetChildren(dbname_, &filenames);
  if (!s.ok()) {
    return s;
  }

  std::vector<uint64_t> logs;
  for (const auto& filename : filenames) {
    uint64_t number = 0;
    FileType type;
    if (ParseFileName(filename, &number, &type)) {
      if (type == FileType::kLogFile) {
        logs.push_back(number);
      }
    }
  }

  if (!current_exists) {
    if (!options_.create_if_missing && logs.empty()) {
      return Status::InvalidArgument(dbname_, "does not exist (create_if_missing is false)");
    }
    if (options_.error_if_exists && (current_exists || !logs.empty())) {
      return Status::InvalidArgument(dbname_, "exists (error_if_exists is true)");
    }
    s = NewDB();
    if (!s.ok()) {
      return s;
    }
  } else {
    if (options_.error_if_exists) {
      return Status::InvalidArgument(dbname_, "exists (error_if_exists is true)");
    }
  }

  versions_ = std::make_unique<VersionSet>(dbname_, &options_, &icmp_);
  s = versions_->Recover();
  if (!s.ok()) {
    return s;
  }

  // Scan directory and mark all file numbers as used in VersionSet
  filenames.clear();
  s = env_->GetChildren(dbname_, &filenames);
  if (!s.ok()) {
    return s;
  }
  logs.clear();
  for (const auto& filename : filenames) {
    uint64_t number = 0;
    FileType type;
    if (ParseFileName(filename, &number, &type)) {
      versions_->MarkFileNumberUsed(number);
      if (type == FileType::kLogFile) {
        logs.push_back(number);
      }
    }
  }

  // Replay only logs numbered at or above versions_->LogNumber()
  std::vector<uint64_t> logs_to_replay;
  for (uint64_t log_num : logs) {
    if (log_num >= versions_->LogNumber()) {
      logs_to_replay.push_back(log_num);
    }
  }
  std::sort(logs_to_replay.begin(), logs_to_replay.end());

  // mem_ must be initialized before replay since replay fills it.
  mem_ = new MemTable(icmp_);
  mem_->Ref();

  // Replay every log in ascending order, tracking highest sequence number seen.
  SequenceNumber max_sequence = 0;
  for (uint64_t log_num : logs_to_replay) {
    s = RecoverLogFile(log_num, &max_sequence);
    if (!s.ok()) {
      return s;
    }
  }

  if (max_sequence > versions_->LastSequence()) {
    versions_->SetLastSequence(max_sequence);
  }
  last_sequence_ = versions_->LastSequence();

  // Create a new log file using VersionSet's file numbering.
  // In Week 2 task, do not advance the log number in the MANIFEST until memtable is flushed to L0.
  logfile_number_ = versions_->NewFileNumber();
  const std::string log_fname = LogFileName(dbname_, logfile_number_);
  s = env_->NewWritableFile(log_fname, &logfile_);
  if (!s.ok()) {
    return s;
  }
  log_ = std::make_unique<log::Writer>(logfile_.get());

  // Sync the directory to durable storage so the new file's directory entry is durable.
  s = env_->SyncDir(dbname_);
  if (!s.ok()) {
    return s;
  }

  return Status::OK();
}

Status DBImpl::RecoverLogFile(uint64_t number, SequenceNumber* max_sequence) {
  const std::string log_fname = LogFileName(dbname_, number);
  std::unique_ptr<SequentialFile> file;
  Status s = env_->NewSequentialFile(log_fname, &file);
  if (!s.ok()) {
    return s;
  }

  class LogReporter : public log::Reader::Reporter {
   public:
    void Corruption(size_t /*bytes*/, const Status& status) override {
      if (status_.ok()) {
        status_ = status;
      }
    }
    const Status& status() const { return status_; }
    bool has_corruption() const { return !status_.ok(); }

   private:
    Status status_;
  };

  LogReporter reporter;
  log::Reader reader(file.get(), &reporter, /*checksum=*/true);

  std::string_view record;
  std::string scratch;
  WriteBatch batch;

  // The corruption policy for Week 1: any reported corruption fails Open.
  // Torn tails are reported silently by the reader, so a normal crash still opens fine;
  // only real damage in the middle of a log fails.
  // Note: in Week 2, MANIFEST and L0 flush will allow retiring logs and keeping prefixes.
  while (reader.ReadRecord(&record, &scratch)) {
    if (reporter.has_corruption()) {
      break;
    }
    if (record.size() < 12) {
      reporter.Corruption(record.size(), Status::Corruption("log record too small"));
      break;
    }
    WriteBatchInternal::SetContents(&batch, record);
    s = WriteBatchInternal::InsertInto(&batch, mem_);
    if (!s.ok()) {
      reporter.Corruption(record.size(), s);
      break;
    }
    const uint32_t count = WriteBatchInternal::Count(&batch);
    if (count > 0) {
      const SequenceNumber seq = WriteBatchInternal::Sequence(&batch);
      const SequenceNumber last_seq = seq + count - 1;
      if (last_seq > *max_sequence) {
        *max_sequence = last_seq;
      }
    }
    if (reporter.has_corruption()) {
      break;
    }
  }

  if (reporter.has_corruption()) {
    return reporter.status();
  }
  return Status::OK();
}

Status DBImpl::Write(const WriteOptions& opts, WriteBatch* batch) {
  if (batch == nullptr) {
    return Status::InvalidArgument("WriteBatch pointer is null");
  }

  std::lock_guard<std::mutex> lock(mutex_);  // Week 1: the mutex is held for the whole write
  // TODO(Week 4): group commit releases the mutex while the leader writes.

  if (!bg_error_.ok()) {
    return bg_error_;
  }

  Status s = batch->Iterate(nullptr);
  if (!s.ok()) {
    return s;
  }

  const uint32_t count = WriteBatchInternal::Count(batch);
  if (count == 0) {
    return Status::OK();
  }

  const SequenceNumber seq = last_sequence_ + 1;
  if (seq + count - 1 > kMaxSequenceNumber) {
    return Status::InvalidArgument("sequence number overflow");
  }

  WriteBatchInternal::SetSequence(batch, seq);

  s = log_->AddRecord(WriteBatchInternal::Contents(batch));
  if (s.ok() && opts.sync) {
    s = logfile_->Sync();
  }
  if (!s.ok()) {
    bg_error_ = s;  // sticky: after one failed log write, every later write returns the same error
    return s;
  }

  s = WriteBatchInternal::InsertInto(batch, mem_);
  STRATA_DCHECK(s.ok());

  last_sequence_ = seq + count - 1;
  if (versions_ != nullptr) {
    versions_->SetLastSequence(last_sequence_);
  }
  return s;
}

Status DBImpl::Get(const ReadOptions& opts, std::string_view key, std::string* value) {
  if (value == nullptr) {
    return Status::InvalidArgument("value pointer is null");
  }

  SequenceNumber snapshot;
  MemTable* mem = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (opts.snapshot != nullptr) {
      snapshot = static_cast<const SnapshotImpl*>(opts.snapshot)->sequence_number();
    } else {
      snapshot = last_sequence_;
    }
    mem = mem_;
    mem->Ref();
  }

  LookupKey lkey(key, snapshot);
  Status s;
  const bool found = mem->Get(lkey, value, &s);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Release the memtable pinned above, not whatever mem_ points to now:
    // once Write can swap mem_ (Week 2), they differ.
    mem->Unref();
  }

  if (!found) {
    // Week 2: check imm_ and L0 files here
    return Status::NotFound("key not found");
  }
  return s;
}

const Snapshot* DBImpl::GetSnapshot() {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshots_.New(last_sequence_);
}

void DBImpl::ReleaseSnapshot(const Snapshot* snapshot) {
  if (snapshot == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  snapshots_.Delete(static_cast<const SnapshotImpl*>(snapshot));
}

std::unique_ptr<Iterator> DBImpl::NewIterator(const ReadOptions& /*opts*/) {
  return NewErrorIterator(Status::NotSupported("NewIterator is not supported in Week 1"));
}

Status DBImpl::Flush() { return Status::NotSupported("Flush is not supported in Week 1"); }

Status DBImpl::CompactRange(std::string_view /*begin*/, std::string_view /*end*/) {
  return Status::NotSupported("CompactRange is not supported in Week 1");
}

bool DBImpl::GetProperty(std::string_view /*name*/, std::string* /*value*/) { return false; }

}  // namespace strata

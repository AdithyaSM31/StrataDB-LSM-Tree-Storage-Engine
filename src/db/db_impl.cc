#include "db/db_impl.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "db/builder.h"
#include "db/db_iter.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "db/table_cache.h"
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
#include "table/merger.h"
#include "table/table_builder.h"
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

namespace {

// Smallest write buffer we accept. A new memtable allocates its skiplist head
// from a 4 KB arena block, so an empty one already reports just over 4 KB; with
// a buffer that small every write became its own flush and its own L0 file.
// LevelDB uses the same 64 KB minimum.
constexpr size_t kMinWriteBufferSize = size_t{64} * 1024;

Options SanitizeOptions(const Options& src) {
  Options result = src;
  result.write_buffer_size = std::max(result.write_buffer_size, kMinWriteBufferSize);
  return result;
}

}  // namespace

struct DBImpl::CompactionState {
  Compaction* const compaction;
  // Entries hidden from every snapshot at or above this sequence may be dropped.
  const SequenceNumber smallest_snapshot;
  // A manual compaction (CompactRange) always rewrites its inputs: a trivial
  // move would carry tombstones and overwritten values down unchanged.
  const bool is_manual;

  struct Output {
    uint64_t number = 0;
    uint64_t file_size = 0;
    std::string smallest;
    std::string largest;
  };
  std::vector<Output> outputs;

  std::unique_ptr<WritableFile> outfile;
  std::unique_ptr<TableBuilder> builder;
  uint64_t total_bytes = 0;

  CompactionState(Compaction* c, SequenceNumber snap, bool manual)
      : compaction(c), smallest_snapshot(snap), is_manual(manual) {}

  Output* current_output() {
    STRATA_DCHECK(!outputs.empty());
    return &outputs.back();
  }
};

DBImpl::DBImpl(const Options& options, const std::string& dbname)
    : env_(options.env == nullptr ? Env::Default() : options.env),
      options_(SanitizeOptions(options)),
      dbname_(dbname),
      icmp_(),
      imm_(nullptr),
      table_cache_(std::make_unique<TableCache>(dbname_, options_, &icmp_)),
      snapshots_() {}

DBImpl::~DBImpl() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shutting_down_.store(true, std::memory_order_release);
    background_work_cv_.notify_all();
  }
  if (background_thread_.joinable()) {
    background_thread_.join();
  }
  if (log_ != nullptr) {
    log_.reset();
  }
  if (logfile_ != nullptr) {
    (void)logfile_->Close();
    logfile_.reset();
  }
  if (imm_ != nullptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    imm_->Unref();
    imm_ = nullptr;
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
    if (options_.error_if_exists && !logs.empty()) {
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
    bool corrupted = false;
    s = RecoverLogFile(log_num, &max_sequence, &corrupted);
    if (!s.ok()) {
      return s;
    }
    if (corrupted) {
      break;
    }
  }

  if (max_sequence > versions_->LastSequence()) {
    versions_->SetLastSequence(max_sequence);
  }

  // Create a new log file using VersionSet's file numbering.
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

  // If the memtable isn't empty, write it to L0 right there in Open, synchronously,
  // with no background thread yet.
  if (!mem_->IsEmpty()) {
    FileMetaData meta;
    meta.number = versions_->NewFileNumber();
    pending_outputs_.insert(meta.number);
    std::unique_ptr<Iterator> iter = mem_->NewIterator();
    s = BuildTable(dbname_, env_, options_, icmp_, iter.get(), &meta);
    pending_outputs_.erase(meta.number);
    if (!s.ok()) {
      return s;
    }
    VersionEdit edit;
    if (meta.file_size > 0) {
      edit.AddFile(0, meta.number, meta.file_size, meta.smallest, meta.largest);
    }
    edit.SetLogNumber(logfile_number_);
    s = versions_->LogAndApply(&edit);
    if (!s.ok()) {
      return s;
    }
    mem_->Unref();
    mem_ = new MemTable(icmp_);
    mem_->Ref();
  } else if (!logs_to_replay.empty()) {
    VersionEdit edit;
    edit.SetLogNumber(logfile_number_);
    s = versions_->LogAndApply(&edit);
    if (!s.ok()) {
      return s;
    }
  }

  RemoveObsoleteFiles();

  background_thread_ = std::thread(&DBImpl::BackgroundThreadMain, this);

  return Status::OK();
}

Status DBImpl::RecoverLogFile(uint64_t number, SequenceNumber* max_sequence, bool* corrupted) {
  *corrupted = false;
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
    if (options_.paranoid_checks) {
      return reporter.status();
    }
    // Keep the prefix: replay stops here, and Recover() discards every later
    // record and log. Remember it so it is visible (GetProperty("strata.recovery")).
    recovery_status_ =
        Status::Corruption(log_fname + ": " + reporter.status().ToString(),
                           "replay stopped here; later records and logs were discarded");
    *corrupted = true;
    return Status::OK();
  }
  return Status::OK();
}

Status DBImpl::SwitchMemTable() {
  const uint64_t new_number = versions_->NewFileNumber();
  const std::string log_fname = LogFileName(dbname_, new_number);
  std::unique_ptr<WritableFile> new_logfile;
  Status s = env_->NewWritableFile(log_fname, &new_logfile);
  if (!s.ok()) {
    versions_->ReuseFileNumber(new_number);
    return s;
  }
  log_.reset();
  if (logfile_ != nullptr) {
    (void)logfile_->Close();
  }
  logfile_ = std::move(new_logfile);
  log_ = std::make_unique<log::Writer>(logfile_.get());
  logfile_number_ = new_number;
  imm_ = mem_;
  has_imm_.store(true, std::memory_order_release);
  mem_ = new MemTable(icmp_);
  mem_->Ref();
  background_work_cv_.notify_all();
  return Status::OK();
}

Status DBImpl::MakeRoomForWrite(std::unique_lock<std::mutex>* lock, bool force) {
  bool allow_delay = !force;
  while (true) {
    if (!bg_error_.ok()) {
      return bg_error_;
    }
    if (allow_delay && versions_->NumLevelFiles(0) >= options_.l0_slowdown_trigger) {
      // At 8 or more L0 files: release lock, sleep 1 ms, retake lock. Do once per write.
      allow_delay = false;
      lock->unlock();
      env_->SleepForMicroseconds(1000);
      lock->lock();
      continue;
    }
    if (!force &&
        (mem_->IsEmpty() || mem_->ApproximateMemoryUsage() <= options_.write_buffer_size)) {
      return Status::OK();
    }
    if (force && mem_->IsEmpty()) {
      return Status::OK();
    }
    if (imm_ != nullptr) {
      background_work_cv_.wait(*lock);
      continue;
    }
    if (versions_->NumLevelFiles(0) >= options_.l0_stop_trigger) {
      background_work_cv_.wait(*lock);
      continue;
    }
    Status s = SwitchMemTable();
    if (!s.ok()) {
      return s;
    }
    force = false;
  }
}

bool DBImpl::HasBackgroundWork() const {
  return shutting_down_.load(std::memory_order_acquire) ||
         (bg_error_.ok() &&
          (imm_ != nullptr || manual_compaction_ != nullptr || versions_->NeedsCompaction()));
}

SequenceNumber DBImpl::SmallestSnapshot() const {
  return snapshots_.empty() ? versions_->LastSequence() : snapshots_.oldest()->sequence_number();
}

void DBImpl::BackgroundThreadMain() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    background_work_cv_.wait(lock, [this] { return HasBackgroundWork(); });
    if (shutting_down_) {
      break;
    }
    if (!bg_error_.ok()) {
      break;
    }

    // Priority 1: Flush imm_
    if (imm_ != nullptr) {
      CompactMemTable();
      background_work_cv_.notify_all();
      continue;
    }

    // Priority 2: Manual compaction
    if (manual_compaction_ != nullptr) {
      ManualCompaction* m = manual_compaction_;
      std::unique_ptr<Compaction> c = versions_->CompactRange(m->level, m->begin, m->end);
      if (c == nullptr) {
        m->done = true;
      } else {
        CompactionState compact(c.get(), SmallestSnapshot(), /*manual=*/true);
        // A failure is recorded in bg_error_, which CompactRange returns.
        (void)DoCompactionWork(&compact, &lock);
        m->done = true;
      }
      manual_compaction_ = nullptr;
      background_work_cv_.notify_all();
      continue;
    }

    // Priority 3: Automatic compaction
    if (versions_->NeedsCompaction()) {
      std::unique_ptr<Compaction> c = versions_->PickCompaction();
      if (c != nullptr) {
        CompactionState compact(c.get(), SmallestSnapshot(), /*manual=*/false);
        (void)DoCompactionWork(&compact, &lock);  // failures go to bg_error_
      }
      background_work_cv_.notify_all();
      continue;
    }
  }
}

void DBImpl::CompactMemTable() {
  // mutex_ held on entry
  FileMetaData meta;
  meta.number = versions_->NewFileNumber();
  pending_outputs_.insert(meta.number);
  Version* base = versions_->current();
  base->Ref();

  MemTable* imm = imm_;
  STRATA_DCHECK(imm != nullptr);
  std::unique_ptr<Iterator> iter = imm->NewIterator();

  mutex_.unlock();

  Status s = BuildTable(dbname_, env_, options_, icmp_, iter.get(), &meta);

  mutex_.lock();

  base->Unref();
  pending_outputs_.erase(meta.number);

  if (s.ok()) {
    VersionEdit edit;
    if (meta.file_size > 0) {
      edit.AddFile(0, meta.number, meta.file_size, meta.smallest, meta.largest);
    }
    edit.SetLogNumber(logfile_number_);  // the log that belongs to mem_, NOT to imm_
    s = versions_->LogAndApply(&edit);
  }

  if (s.ok()) {
    imm_->Unref();
    imm_ = nullptr;
    has_imm_.store(false, std::memory_order_release);
    RemoveObsoleteFiles();
  } else {
    bg_error_ = s;  // sticky
  }
}

void DBImpl::RemoveObsoleteFiles() {
  std::set<uint64_t> live_tables = pending_outputs_;
  versions_->AddLiveFiles(&live_tables);

  std::vector<std::string> filenames;
  Status s = env_->GetChildren(dbname_, &filenames);
  if (!s.ok()) {
    return;
  }

  for (const auto& filename : filenames) {
    uint64_t number = 0;
    FileType type;
    if (ParseFileName(filename, &number, &type)) {
      bool keep = true;
      switch (type) {
        case FileType::kLogFile:
          keep = (number >= versions_->LogNumber());
          break;
        case FileType::kDescriptorFile:
          keep = (number >= versions_->ManifestFileNumber());
          break;
        case FileType::kTableFile:
          keep = (live_tables.find(number) != live_tables.end());
          break;
        case FileType::kTempFile:
          keep = false;
          break;
        case FileType::kCurrentFile:
        case FileType::kLockFile:
          keep = true;
          break;
      }
      if (!keep) {
        if (type == FileType::kTableFile && table_cache_ != nullptr) {
          table_cache_->Evict(number);
        }
        (void)env_->RemoveFile(dbname_ + "/" + filename);
      }
    }
  }
}

WriteBatch* DBImpl::BuildBatchGroup(Writer** last_writer) {
  STRATA_DCHECK(!writers_.empty());
  Writer* first = writers_.front();
  WriteBatch* result = first->batch;
  STRATA_DCHECK(result != nullptr);

  size_t size = first->batch->ApproximateSize();

  // Allow the group to grow up to a maximum size (1 MB), but if the
  // leader write is small (<= 128 KB), stop at leader's size + 128 KB.
  size_t max_size = 1 << 20;  // 1 MB
  if (size <= (128 << 10)) {
    max_size = size + (128 << 10);
  }

  *last_writer = first;
  auto iter = writers_.begin();
  ++iter;  // advance past first
  for (; iter != writers_.end(); ++iter) {
    Writer* w = *iter;
    if (w->sync && !first->sync) {
      // Do not include a sync write into a batch handled by a non-sync write.
      break;
    }
    if (w->batch == nullptr) {
      // Flush forces a memtable switch as leader; do not include in group.
      break;
    }
    size += w->batch->ApproximateSize();
    if (size > max_size) {
      break;
    }
    if (result == first->batch) {
      result = &tmp_batch_;
      STRATA_DCHECK(WriteBatchInternal::Count(result) == 0);
      WriteBatchInternal::Append(result, first->batch);
    }
    WriteBatchInternal::Append(result, w->batch);
    *last_writer = w;
  }
  return result;
}

Status DBImpl::Write(const WriteOptions& opts, WriteBatch* batch) {
  if (batch != nullptr) {
    Status s = batch->Iterate(nullptr);
    if (!s.ok()) {
      return s;
    }
    if (WriteBatchInternal::Count(batch) == 0) {
      return Status::OK();
    }
  }

  Writer w;
  w.batch = batch;
  w.sync = opts.sync;
  w.done = false;

  std::unique_lock<std::mutex> lock(mutex_);
  writers_.push_back(&w);
  while (!w.done && &w != writers_.front()) {
    w.cv.wait(lock);
  }
  if (w.done) {
    return w.status;
  }

  // I am the leader.
  Status s = MakeRoomForWrite(&lock, /*force=*/w.batch == nullptr);
  uint64_t last_sequence = versions_->LastSequence();
  Writer* last_writer = &w;
  if (s.ok() && w.batch != nullptr) {
    WriteBatch* updates = BuildBatchGroup(&last_writer);
    const uint32_t count = WriteBatchInternal::Count(updates);
    if (last_sequence + count > kMaxSequenceNumber) {
      s = Status::InvalidArgument("sequence number overflow");
    } else {
      WriteBatchInternal::SetSequence(updates, last_sequence + 1);
      last_sequence += count;
      {
        lock.unlock();
        s = log_->AddRecord(WriteBatchInternal::Contents(updates));
        if (s.ok() && w.sync) {
          s = logfile_->Sync();
        }
        if (s.ok()) {
          s = WriteBatchInternal::InsertInto(updates, mem_);
        }
        lock.lock();
      }
      if (!s.ok()) {
        bg_error_ = s;  // sticky
        background_work_cv_.notify_all();
      } else {
        versions_->SetLastSequence(last_sequence);
      }
    }
    if (updates == &tmp_batch_) {
      tmp_batch_.Clear();
    }
  }

  while (true) {
    Writer* ready = writers_.front();
    writers_.pop_front();
    if (ready != &w) {
      ready->status = s;
      ready->done = true;
      ready->cv.notify_one();
    }
    if (ready == last_writer) {
      break;
    }
  }

  if (!writers_.empty()) {
    writers_.front()->cv.notify_one();
  }

  return s;
}

Status DBImpl::Get(const ReadOptions& opts, std::string_view key, std::string* value) {
  if (value == nullptr) {
    return Status::InvalidArgument("value pointer is null");
  }

  SequenceNumber snapshot;
  MemTable* mem = nullptr;
  MemTable* imm = nullptr;
  Version* current = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (opts.snapshot != nullptr) {
      snapshot = static_cast<const SnapshotImpl*>(opts.snapshot)->sequence_number();
    } else {
      snapshot = versions_->LastSequence();
    }
    mem = mem_;
    imm = imm_;
    current = versions_->current();
    mem->Ref();
    if (imm != nullptr) {
      imm->Ref();
    }
    current->Ref();
  }

  LookupKey lkey(key, snapshot);
  Status s;
  // Newest data first: the active memtable, the one being flushed, then files.
  // A memtable "has the answer" for a value or a tombstone alike.
  const bool in_memory = mem->Get(lkey, value, &s) || (imm != nullptr && imm->Get(lkey, value, &s));
  if (!in_memory) {
    s = current->Get(lkey, value, table_cache_.get());
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    mem->Unref();
    if (imm != nullptr) {
      imm->Unref();
    }
    current->Unref();
  }

  return s;
}

const Snapshot* DBImpl::GetSnapshot() {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshots_.New(versions_->LastSequence());
}

void DBImpl::ReleaseSnapshot(const Snapshot* snapshot) {
  if (snapshot == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  snapshots_.Delete(static_cast<const SnapshotImpl*>(snapshot));
}

std::unique_ptr<Iterator> DBImpl::NewIterator(const ReadOptions& opts) {
  SequenceNumber sequence = 0;
  std::vector<std::unique_ptr<Iterator>> list;
  MemTable* mem = nullptr;
  MemTable* imm = nullptr;
  Version* current = nullptr;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (opts.snapshot != nullptr) {
      sequence = static_cast<const SnapshotImpl*>(opts.snapshot)->sequence_number();
    } else {
      sequence = versions_->LastSequence();
    }

    mem = mem_;
    imm = imm_;
    current = versions_->current();

    mem->Ref();
    if (imm != nullptr) {
      imm->Ref();
    }
    current->Ref();

    list.push_back(mem->NewIterator());
    if (imm != nullptr) {
      list.push_back(imm->NewIterator());
    }

    for (FileMetaData* f : current->files(0)) {
      list.push_back(table_cache_->NewIterator(f->number, f->file_size));
    }

    for (int level = 1; level < kNumLevels; ++level) {
      if (!current->files(level).empty()) {
        list.push_back(NewLevelFileIterator(&icmp_, table_cache_.get(), current->files(level)));
      }
    }
  }

  auto internal_iter = NewMergingIterator(&icmp_, std::move(list));
  auto db_iter = NewDBIterator(BytewiseComparator(), std::move(internal_iter), sequence);
  db_iter->RegisterCleanup([this, mem, imm, current]() {
    std::lock_guard<std::mutex> lock(mutex_);
    mem->Unref();
    if (imm != nullptr) {
      imm->Unref();
    }
    current->Unref();
  });
  return db_iter;
}

Status DBImpl::Flush() {
  Status s = Write(WriteOptions(), nullptr);
  if (!s.ok()) {
    return s;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  while (imm_ != nullptr && bg_error_.ok()) {
    background_work_cv_.wait(lock);
  }
  return bg_error_;
}

void DBImpl::TEST_CompactRange(int level, const std::string* begin, const std::string* end) {
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    while (manual_compaction_ != nullptr && bg_error_.ok() && !shutting_down_) {
      background_work_cv_.wait(lock);
    }
    if (!bg_error_.ok() || shutting_down_) {
      break;
    }
    std::vector<FileMetaData*> overlapping;
    versions_->GetOverlappingInputs(level, begin, end, &overlapping);
    if (overlapping.empty()) {
      break;
    }
    ManualCompaction manual;
    manual.level = level;
    manual.done = false;
    manual.begin = begin;
    manual.end = end;
    manual_compaction_ = &manual;
    background_work_cv_.notify_all();
    while (!manual.done && bg_error_.ok() && !shutting_down_) {
      background_work_cv_.wait(lock);
    }
    if (manual_compaction_ == &manual) {
      manual_compaction_ = nullptr;
    }
  }
}

int DBImpl::TEST_NumLevelFiles(int level) {
  std::lock_guard<std::mutex> lock(mutex_);
  return versions_->NumLevelFiles(level);
}

std::vector<FileMetaData> DBImpl::TEST_LevelFiles(int level) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<FileMetaData> result;
  for (const FileMetaData* f : versions_->current()->files(level)) {
    result.push_back(*f);
  }
  return result;
}

std::string DBImpl::TEST_CompactPointer(int level) {
  std::lock_guard<std::mutex> lock(mutex_);
  return versions_->compact_pointer(level);
}

int DBImpl::TEST_WritersCount() {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<int>(writers_.size());
}

SequenceNumber DBImpl::TEST_LastSequence() {
  std::lock_guard<std::mutex> lock(mutex_);
  return versions_->LastSequence();
}

void DBImpl::TEST_SetLastSequence(SequenceNumber seq) {
  std::lock_guard<std::mutex> lock(mutex_);
  versions_->SetLastSequence(seq);
}

Status DBImpl::CompactRange(std::string_view begin, std::string_view end) {
  int max_level_with_files = 1;
  std::string begin_str(begin);
  std::string end_str(end);
  const std::string* begin_ptr = begin.empty() ? nullptr : &begin_str;
  const std::string* end_ptr = end.empty() ? nullptr : &end_str;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int level = 1; level < kNumLevels; ++level) {
      std::vector<FileMetaData*> overlapping;
      versions_->GetOverlappingInputs(level, begin_ptr, end_ptr, &overlapping);
      if (!overlapping.empty()) {
        max_level_with_files = level;
      }
    }
  }

  Status s = Flush();
  if (!s.ok()) {
    return s;
  }

  for (int level = 0; level < max_level_with_files; ++level) {
    TEST_CompactRange(level, begin_ptr, end_ptr);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return bg_error_;
}

std::unique_ptr<Iterator> DBImpl::MakeInputIterator(Compaction* c) {
  std::vector<std::unique_ptr<Iterator>> children;
  if (c->level() == 0) {
    // L0 files overlap each other: each one needs its own iterator.
    for (const FileMetaData* f : c->inputs(0)) {
      children.push_back(table_cache_->NewIterator(f->number, f->file_size));
    }
  } else if (c->num_input_files(0) > 0) {
    children.push_back(NewLevelFileIterator(&icmp_, table_cache_.get(), c->inputs(0)));
  }
  if (c->num_input_files(1) > 0) {
    children.push_back(NewLevelFileIterator(&icmp_, table_cache_.get(), c->inputs(1)));
  }
  return NewMergingIterator(&icmp_, std::move(children));
}

Status DBImpl::OpenCompactionOutputFile(CompactionState* compact) {
  STRATA_DCHECK(compact != nullptr);
  STRATA_DCHECK(compact->builder == nullptr);

  uint64_t file_number = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    file_number = versions_->NewFileNumber();
    pending_outputs_.insert(file_number);
  }

  CompactionState::Output out;
  out.number = file_number;
  compact->outputs.push_back(out);

  const std::string fname = TableFileName(dbname_, file_number);
  std::unique_ptr<WritableFile> file;
  Status s = env_->NewWritableFile(fname, &file);
  if (!s.ok()) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_outputs_.erase(file_number);
    compact->outputs.pop_back();
    return s;
  }

  compact->outfile = std::move(file);
  compact->builder = std::make_unique<TableBuilder>(options_, &icmp_, compact->outfile.get());
  compact->compaction->ResetOutputOverlap();
  return Status::OK();
}

Status DBImpl::FinishCompactionOutputFile(CompactionState* compact) {
  STRATA_DCHECK(compact != nullptr);
  STRATA_DCHECK(compact->outfile != nullptr);
  STRATA_DCHECK(compact->builder != nullptr);

  const uint64_t output_number = compact->current_output()->number;
  const std::string fname = TableFileName(dbname_, output_number);

  Status s = compact->builder->Finish();
  if (s.ok()) {
    compact->current_output()->file_size = compact->builder->FileSize();
    compact->total_bytes += compact->current_output()->file_size;
    s = compact->outfile->Sync();
  }
  Status close_s = compact->outfile->Close();
  if (s.ok() && !close_s.ok()) {
    s = close_s;
  }
  compact->builder.reset();
  compact->outfile.reset();

  if (s.ok() && compact->current_output()->file_size > 0) {
    s = env_->SyncDir(dbname_);
  }

  if (s.ok() && compact->current_output()->file_size > 0) {
    std::unique_ptr<RandomAccessFile> rfile;
    s = env_->NewRandomAccessFile(fname, &rfile);
    if (s.ok()) {
      std::unique_ptr<Table> table;
      s = Table::Open(options_, &icmp_, std::move(rfile), compact->current_output()->file_size,
                      &table);
    }
  }

  if (!s.ok()) {
    (void)env_->RemoveFile(fname);
  }
  return s;
}

Status DBImpl::InstallCompactionResults(CompactionState* compact) {
  VersionEdit edit;
  compact->compaction->AddInputDeletions(&edit);
  const int level = compact->compaction->level();
  for (const auto& out : compact->outputs) {
    if (out.file_size > 0) {
      edit.AddFile(level + 1, out.number, out.file_size, out.smallest, out.largest);
    }
  }

  std::string largest_key;
  for (size_t i = 0; i < compact->compaction->num_input_files(0); ++i) {
    const std::string& key = compact->compaction->input(0, i)->largest;
    if (largest_key.empty() || icmp_.Compare(key, largest_key) > 0) {
      largest_key = key;
    }
  }
  if (!largest_key.empty()) {
    edit.SetCompactPointer(level, largest_key);
  }

  return versions_->LogAndApply(&edit);
}

Status DBImpl::DoCompactionWork(CompactionState* compact, std::unique_lock<std::mutex>* lock) {
  if (!compact->is_manual && compact->compaction->IsTrivialMove()) {
    STRATA_DCHECK(compact->compaction->num_input_files(0) == 1);
    FileMetaData* f = compact->compaction->input(0, 0);
    VersionEdit edit;
    edit.RemoveFile(compact->compaction->level(), f->number);
    edit.AddFile(compact->compaction->level() + 1, f->number, f->file_size, f->smallest,
                 f->largest);
    edit.SetCompactPointer(compact->compaction->level(), f->largest);

    Status s = versions_->LogAndApply(&edit);
    if (!s.ok()) {
      bg_error_ = s;
    }
    compact->compaction->ReleaseInputVersion();
    RemoveObsoleteFiles();
    return s;
  }

  std::unique_ptr<Iterator> input = MakeInputIterator(compact->compaction);

  lock->unlock();

  input->SeekToFirst();
  Status status = input->status();

  std::string current_user_key;
  bool has_current_user_key = false;
  SequenceNumber last_sequence_for_key = kMaxSequenceNumber;

  while (input->Valid() && !shutting_down_.load(std::memory_order_acquire)) {
    if (has_imm_.load(std::memory_order_relaxed)) {
      lock->lock();
      if (imm_ != nullptr) {
        CompactMemTable();
        background_work_cv_.notify_all();
      }
      lock->unlock();
    }

    std::string_view key = input->key();
    ParsedInternalKey ikey;
    bool drop = false;
    if (!ParseInternalKey(key, &ikey)) {
      // Keep a key we cannot parse: dropping it would hide the corruption,
      // while keeping it lets a later read or check report it.
      current_user_key.clear();
      has_current_user_key = false;
      last_sequence_for_key = kMaxSequenceNumber;
    } else {
      if (!has_current_user_key || ikey.user_key != current_user_key) {
        current_user_key = std::string(ikey.user_key);
        has_current_user_key = true;
        last_sequence_for_key = kMaxSequenceNumber;
      }
      // Rule A: a newer version of this key is already visible to every
      // snapshot, so this one can never be read.
      // Rule B: a tombstone no snapshot needs, with no older data below the
      // output level that it would still have to hide.
      drop = last_sequence_for_key <= compact->smallest_snapshot ||
             (ikey.type == kTypeDeletion && ikey.sequence <= compact->smallest_snapshot &&
              compact->compaction->IsBaseLevelForKey(ikey.user_key));
      last_sequence_for_key = ikey.sequence;
    }

    if (!drop) {
      if (compact->builder == nullptr) {
        status = OpenCompactionOutputFile(compact);
        if (!status.ok()) {
          break;
        }
      }
      if (compact->builder->NumEntries() == 0) {
        compact->current_output()->smallest = std::string(key);
      }
      compact->current_output()->largest = std::string(key);
      compact->builder->Add(key, input->value());
    }

    input->Next();

    // Cut an output file only between two different user keys: every version
    // of a key must stay in one file, or L1+ files would overlap.
    if (compact->builder != nullptr && input->Valid()) {
      ParsedInternalKey next_ikey;
      if (ParseInternalKey(input->key(), &next_ikey) &&
          (!has_current_user_key || next_ikey.user_key != current_user_key)) {
        if (compact->builder->FileSize() >= compact->compaction->max_output_file_size() ||
            compact->compaction->ShouldStopBefore(input->key())) {
          status = FinishCompactionOutputFile(compact);
          if (!status.ok()) {
            break;
          }
        }
      }
    }
  }

  if (status.ok()) {
    status = input->status();
  }

  if (status.ok() && compact->builder != nullptr) {
    status = FinishCompactionOutputFile(compact);
  }

  if (!status.ok() && compact->builder != nullptr) {
    compact->builder->Abandon();
    (void)compact->outfile->Close();
    compact->builder.reset();
    compact->outfile.reset();
  }

  lock->lock();

  if (status.ok() && shutting_down_.load(std::memory_order_acquire)) {
    status = Status::IOError("compaction aborted by shutdown");
  }

  if (status.ok()) {
    status = InstallCompactionResults(compact);
  }

  compact->compaction->ReleaseInputVersion();

  if (!status.ok()) {
    if (!shutting_down_.load(std::memory_order_acquire)) {
      bg_error_ = status;
    }
    for (const auto& out : compact->outputs) {
      (void)env_->RemoveFile(TableFileName(dbname_, out.number));
    }
  }

  // Only now, with the outputs either installed in the current version or
  // removed, may RemoveObsoleteFiles see them.
  for (const auto& out : compact->outputs) {
    pending_outputs_.erase(out.number);
  }
  RemoveObsoleteFiles();
  return status;
}

bool DBImpl::GetProperty(std::string_view name, std::string* value) {
  if (value == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (name == "strata.recovery") {
    // "OK", or why the last Open stopped replaying the WAL early.
    *value = recovery_status_.ok() ? "OK" : recovery_status_.ToString();
    return true;
  }
  return false;  // "strata.stats" arrives in Week 4
}

}  // namespace strata

#ifndef STRATA_DB_DB_IMPL_H_
#define STRATA_DB_DB_IMPL_H_

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>

#include "db/dbformat.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "log/log_writer.h"
#include "memtable/memtable.h"
#include "strata/db.h"
#include "strata/env.h"
#include "strata/options.h"
#include "strata/status.h"

namespace strata {

class SnapshotImpl : public Snapshot {
 public:
  SequenceNumber sequence_number() const { return sequence_number_; }

 private:
  friend class SnapshotList;
  SequenceNumber sequence_number_ = 0;
  SnapshotImpl* prev_ = nullptr;
  SnapshotImpl* next_ = nullptr;
};

class SnapshotList {
 public:
  SnapshotList() {
    list_.prev_ = &list_;
    list_.next_ = &list_;
  }

  ~SnapshotList() {
    while (!empty()) {
      Delete(oldest());
    }
  }

  SnapshotList(const SnapshotList&) = delete;
  SnapshotList& operator=(const SnapshotList&) = delete;

  bool empty() const { return list_.next_ == &list_; }
  SnapshotImpl* oldest() const { return list_.next_; }
  SnapshotImpl* newest() const { return list_.prev_; }

  SnapshotImpl* New(SequenceNumber seq) {
    auto* s = new SnapshotImpl();
    s->sequence_number_ = seq;
    s->next_ = &list_;
    s->prev_ = list_.prev_;
    s->prev_->next_ = s;
    s->next_->prev_ = s;
    return s;
  }

  void Delete(const SnapshotImpl* s) {
    auto* non_const_s = const_cast<SnapshotImpl*>(s);
    non_const_s->prev_->next_ = non_const_s->next_;
    non_const_s->next_->prev_ = non_const_s->prev_;
    delete non_const_s;
  }

 private:
  SnapshotImpl list_;
};

class DBImpl : public DB {
 public:
  DBImpl(const Options& options, const std::string& dbname);
  ~DBImpl() override;

  DBImpl(const DBImpl&) = delete;
  DBImpl& operator=(const DBImpl&) = delete;

  Status Write(const WriteOptions& opts, WriteBatch* batch) override;
  Status Get(const ReadOptions& opts, std::string_view key, std::string* value) override;
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& opts) override;

  const Snapshot* GetSnapshot() override;
  void ReleaseSnapshot(const Snapshot* snapshot) override;

  Status Flush() override;
  Status CompactRange(std::string_view begin, std::string_view end) override;
  bool GetProperty(std::string_view name, std::string* value) override;

  Status Recover();  // called by DB::Open

 private:
  Status NewDB();
  Status RecoverLogFile(uint64_t number, SequenceNumber* max_sequence, bool* corrupted);
  Status MakeRoomForWrite(std::unique_lock<std::mutex>* lock);
  Status SwitchMemTable();
  void BackgroundThreadMain();
  void CompactMemTable();
  void RemoveObsoleteFiles();

  Env* const env_;
  const Options options_;
  const std::string dbname_;
  const InternalKeyComparator icmp_;

  std::mutex mutex_;
  std::unique_ptr<FileLock> db_lock_;
  std::unique_ptr<VersionSet> versions_;  // guarded by mutex_
  MemTable* mem_ = nullptr;               // Ref'd; guarded by mutex_
  MemTable* imm_ = nullptr;               // being flushed; Ref'd; guarded by mutex_
  std::unique_ptr<WritableFile> logfile_;
  std::unique_ptr<log::Writer> log_;
  uint64_t logfile_number_ = 0;
  Status bg_error_;  // sticky; guarded by mutex_
  // Set if Open stopped replaying the WAL at a corruption (paranoid_checks off).
  Status recovery_status_;  // guarded by mutex_
  std::unique_ptr<TableCache> table_cache_;
  SnapshotList snapshots_;  // guarded by mutex_

  std::thread background_thread_;
  std::condition_variable background_work_cv_;  // "there is work" / "work finished"
  bool shutting_down_ = false;                  // guarded by mutex_
  std::set<uint64_t> pending_outputs_;          // table files being written: never delete these
};

}  // namespace strata

#endif  // STRATA_DB_DB_IMPL_H_

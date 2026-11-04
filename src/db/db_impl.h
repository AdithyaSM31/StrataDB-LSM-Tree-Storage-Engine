#ifndef STRATA_DB_DB_IMPL_H_
#define STRATA_DB_DB_IMPL_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "db/dbformat.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "log/log_writer.h"
#include "memtable/memtable.h"
#include "strata/db.h"
#include "strata/env.h"
#include "strata/options.h"
#include "strata/status.h"
#include "strata/write_batch.h"

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

  // Test helpers
  void TEST_CompactRange(int level, const std::string* begin, const std::string* end);
  int TEST_NumLevelFiles(int level);
  // Copies of the current version's file metadata for one level.
  std::vector<FileMetaData> TEST_LevelFiles(int level);
  // The round-robin compaction pointer for a level ("" if never set).
  std::string TEST_CompactPointer(int level);
  int TEST_WritersCount();
  SequenceNumber TEST_LastSequence();
  void TEST_SetLastSequence(SequenceNumber seq);
  BlockCache* TEST_block_cache() const { return block_cache_.get(); }
  TableCache* TEST_table_cache() const { return table_cache_.get(); }
  const Options& TEST_options() const { return options_; }

 private:
  struct Writer {
    WriteBatch* batch = nullptr;  // nullptr means "just make room" (used by Flush)
    bool sync = false;
    bool done = false;
    Status status;
    std::condition_variable cv;
  };

  Status NewDB();
  Status RecoverLogFile(uint64_t number, SequenceNumber* max_sequence, bool* corrupted);
  Status MakeRoomForWrite(std::unique_lock<std::mutex>* lock, bool force = false);
  // The write path behind Write and Flush. A nullptr batch makes the caller
  // queue like any writer and, once it leads, force a memtable switch.
  Status WriteImpl(const WriteOptions& opts, WriteBatch* batch);
  WriteBatch* BuildBatchGroup(Writer** last_writer);
  Status SwitchMemTable();
  void BackgroundThreadMain();
  bool HasBackgroundWork() const;
  void CompactMemTable();
  void RemoveObsoleteFiles();

  struct CompactionState;
  struct ManualCompaction {
    int level = 0;
    bool done = false;
    const std::string* begin = nullptr;
    const std::string* end = nullptr;
  };

  SequenceNumber SmallestSnapshot() const;  // REQUIRES: mutex_ held
  Status DoCompactionWork(CompactionState* compact, std::unique_lock<std::mutex>* lock);
  Status OpenCompactionOutputFile(CompactionState* compact);
  Status FinishCompactionOutputFile(CompactionState* compact);
  Status InstallCompactionResults(CompactionState* compact);
  std::unique_ptr<Iterator> MakeInputIterator(Compaction* c);
  void RecordBackgroundError(const Status& s);  // REQUIRES: mutex_ held

  Env* const env_;
  const Options options_;
  const std::string dbname_;
  const InternalKeyComparator icmp_;

  std::mutex mutex_;
  std::deque<Writer*> writers_;  // guarded by mutex_
  WriteBatch tmp_batch_;         // the merged group; only the leader touches it
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
  std::unique_ptr<BlockCache> block_cache_;
  std::unique_ptr<TableCache> table_cache_;
  SnapshotList snapshots_;  // guarded by mutex_

  std::thread background_thread_;
  std::condition_variable background_work_cv_;  // "there is work" / "work finished"
  std::atomic<bool> shutting_down_{false};
  std::atomic<bool> has_imm_{false};
  std::set<uint64_t> pending_outputs_;             // table files being written: never delete these
  ManualCompaction* manual_compaction_ = nullptr;  // guarded by mutex_

  struct CompactionStats {
    uint64_t count = 0;
    uint64_t bytes_read = 0;
    uint64_t bytes_written = 0;
  };
  CompactionStats compaction_stats_[kNumLevels];  // guarded by mutex_
  uint64_t flush_count_ = 0;                      // guarded by mutex_
  uint64_t flush_bytes_ = 0;                      // guarded by mutex_
  uint64_t user_bytes_written_ = 0;               // guarded by mutex_
};

}  // namespace strata

#endif  // STRATA_DB_DB_IMPL_H_

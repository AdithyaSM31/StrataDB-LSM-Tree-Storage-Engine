#ifndef STRATA_DB_VERSION_SET_H_
#define STRATA_DB_VERSION_SET_H_

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "db/dbformat.h"
#include "db/version_edit.h"
#include "log/log_writer.h"
#include "strata/env.h"
#include "strata/options.h"
#include "strata/status.h"
#include "util/check.h"

namespace strata {

class Compaction;
class TableCache;
class VersionSet;

class Version {
 public:
  Version() = default;
  ~Version();

  Version(const Version&) = delete;
  Version& operator=(const Version&) = delete;

  void Ref() { ++refs_; }
  void Unref();

  Status Get(const LookupKey& key, std::string* value, TableCache* cache) const;

  const std::vector<FileMetaData*>& files(int level) const {
    STRATA_DCHECK(level >= 0 && level < kNumLevels);
    return files_[level];
  }

  int NumFiles(int level) const {
    STRATA_DCHECK(level >= 0 && level < kNumLevels);
    return static_cast<int>(files_[level].size());
  }

  int NumLevelFiles(int level) const { return NumFiles(level); }

  double compaction_score() const { return compaction_score_; }
  int compaction_level() const { return compaction_level_; }

#if STRATA_DEBUG_CHECKS
  void CheckInvariants(const InternalKeyComparator* icmp) const;
#endif

  std::string DebugString() const;

 private:
  friend class VersionSet;

  Version* next_ = this;
  Version* prev_ = this;
  int refs_ = 0;
  double compaction_score_ = -1.0;
  int compaction_level_ = -1;
  std::vector<FileMetaData*> files_[kNumLevels];
};

class VersionSet {
 public:
  VersionSet(std::string dbname, const Options* options, const InternalKeyComparator* icmp);
  ~VersionSet();

  VersionSet(const VersionSet&) = delete;
  VersionSet& operator=(const VersionSet&) = delete;

  // Apply *edit to the current version to form a new version that is
  // both saved to persistent state and installed as the new current
  // version.
  Status LogAndApply(VersionEdit* edit);

  // Recover the last saved version from persistent storage.
  Status Recover();

  Version* current() const { return current_; }

  uint64_t ManifestFileNumber() const { return manifest_file_number_; }
  uint64_t LogNumber() const { return log_number_; }
  uint64_t NextFileNumber() const { return next_file_number_; }
  SequenceNumber LastSequence() const { return last_sequence_; }

  void SetLastSequence(SequenceNumber s) {
    if (s > last_sequence_) {
      last_sequence_ = s;
    }
  }

  uint64_t NewFileNumber() { return next_file_number_++; }

  void MarkFileNumberUsed(uint64_t number) {
    if (next_file_number_ <= number) {
      next_file_number_ = number + 1;
    }
  }

  void ReuseFileNumber(uint64_t file_number) {
    if (next_file_number_ == file_number + 1) {
      next_file_number_ = file_number;
    }
  }

  const InternalKeyComparator& icmp() const { return *icmp_; }

  void AddLiveFiles(std::set<uint64_t>* live) const;
  int NumLevelFiles(int level) const { return current_->NumFiles(level); }

  bool NeedsCompaction() const { return current_->compaction_score() >= 1.0; }

  const std::string& compact_pointer(int level) const {
    STRATA_DCHECK(level >= 0 && level < kNumLevels);
    return compact_pointer_[level];
  }

  std::unique_ptr<Compaction> PickCompaction();
  std::unique_ptr<Compaction> CompactRange(int level, const std::string* begin,
                                           const std::string* end);

  void GetRange(const std::vector<FileMetaData*>& inputs, std::string* smallest_uk,
                std::string* largest_uk) const;
  void GetRange2(const std::vector<FileMetaData*>& inputs1,
                 const std::vector<FileMetaData*>& inputs2, std::string* smallest_uk,
                 std::string* largest_uk) const;
  void GetOverlappingInputs(int level, const std::string* user_begin, const std::string* user_end,
                            std::vector<FileMetaData*>* inputs) const;
  void GetOverlappingInputs(int level, const std::string& user_begin, const std::string& user_end,
                            std::vector<FileMetaData*>* inputs) const {
    GetOverlappingInputs(level, &user_begin, &user_end, inputs);
  }

 private:
  class Builder;
  friend class Builder;

  void AppendVersion(Version* v);
  void Finalize(Version* v);
  uint64_t MaxBytesForLevel(int level) const;
  void SetupOtherInputs(Compaction* c);

  Env* const env_;
  const Options* const options_;
  const std::string dbname_;
  const InternalKeyComparator* const icmp_;

  uint64_t next_file_number_ = 2;
  uint64_t manifest_file_number_ = 0;
  uint64_t last_sequence_ = 0;
  uint64_t log_number_ = 0;

  std::string compact_pointer_[kNumLevels];

  std::unique_ptr<WritableFile> descriptor_file_;
  std::unique_ptr<log::Writer> descriptor_log_;

  Version dummy_versions_;
  Version* current_ = nullptr;
};

class Compaction {
 public:
  Compaction(const Options* options, int level);
  ~Compaction();

  int level() const { return level_; }

  Version* input_version() const { return input_version_; }
  void SetInputVersion(Version* v);

  size_t num_input_files(int which) const {
    STRATA_DCHECK(which == 0 || which == 1);
    return inputs_[which].size();
  }
  FileMetaData* input(int which, size_t i) const {
    STRATA_DCHECK(which == 0 || which == 1);
    STRATA_DCHECK(i < inputs_[which].size());
    return inputs_[which][i];
  }
  const std::vector<FileMetaData*>& inputs(int which) const {
    STRATA_DCHECK(which == 0 || which == 1);
    return inputs_[which];
  }

  const std::vector<FileMetaData*>& grandparents() const { return grandparents_; }

  uint64_t max_output_file_size() const { return max_output_file_size_; }

  bool IsTrivialMove() const;
  void AddInputDeletions(VersionEdit* edit);
  bool IsBaseLevelForKey(std::string_view user_key);
  bool ShouldStopBefore(std::string_view internal_key);
  void ResetOutputOverlap();

 private:
  friend class VersionSet;

  const Options* options_;
  int level_;
  uint64_t max_output_file_size_;
  Version* input_version_ = nullptr;
  const InternalKeyComparator* icmp_ = nullptr;

  std::vector<FileMetaData*> inputs_[2];
  std::vector<FileMetaData*> grandparents_;

  size_t grandparent_index_ = 0;
  bool seen_key_ = false;
  uint64_t overlapped_bytes_ = 0;
};

}  // namespace strata

#endif  // STRATA_DB_VERSION_SET_H_

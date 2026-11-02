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

  std::string DebugString() const;

 private:
  friend class VersionSet;

  Version* next_ = this;
  Version* prev_ = this;
  int refs_ = 0;
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

 private:
  class Builder;
  friend class Builder;

  void AppendVersion(Version* v);

  Env* const env_;
  const std::string dbname_;
  const InternalKeyComparator* const icmp_;

  uint64_t next_file_number_ = 2;
  uint64_t manifest_file_number_ = 0;
  uint64_t last_sequence_ = 0;
  uint64_t log_number_ = 0;

  std::unique_ptr<WritableFile> descriptor_file_;
  std::unique_ptr<log::Writer> descriptor_log_;

  Version dummy_versions_;
  Version* current_ = nullptr;
};

}  // namespace strata

#endif  // STRATA_DB_VERSION_SET_H_

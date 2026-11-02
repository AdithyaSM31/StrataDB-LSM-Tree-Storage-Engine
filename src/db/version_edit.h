#ifndef STRATA_DB_VERSION_EDIT_H_
#define STRATA_DB_VERSION_EDIT_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "db/dbformat.h"
#include "strata/status.h"

namespace strata {

inline constexpr int kNumLevels = 7;

struct FileMetaData {
  uint64_t number = 0;
  uint64_t file_size = 0;
  std::string smallest;  // internal key
  std::string largest;   // internal key
  int refs = 0;          // how many Versions hold this file
};

class VersionEdit {
 public:
  VersionEdit() = default;
  ~VersionEdit() = default;

  void Clear();

  void SetComparatorName(std::string_view name);
  void SetLogNumber(uint64_t n);
  void SetNextFile(uint64_t n);
  void SetLastSequence(SequenceNumber s);
  void SetCompactPointer(int level, std::string_view key);
  void AddFile(int level, uint64_t number, uint64_t size, std::string_view smallest,
               std::string_view largest);
  void RemoveFile(int level, uint64_t number);

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(std::string_view src);

  bool has_comparator() const { return has_comparator_; }
  const std::string& comparator() const { return comparator_; }

  bool has_log_number() const { return has_log_number_; }
  uint64_t log_number() const { return log_number_; }

  bool has_next_file_number() const { return has_next_file_number_; }
  uint64_t next_file_number() const { return next_file_number_; }

  bool has_last_sequence() const { return has_last_sequence_; }
  SequenceNumber last_sequence() const { return last_sequence_; }

  const std::vector<std::pair<int, std::string>>& compact_pointers() const {
    return compact_pointers_;
  }
  const std::vector<std::pair<int, uint64_t>>& deleted_files() const { return deleted_files_; }
  const std::vector<std::pair<int, FileMetaData>>& new_files() const { return new_files_; }

  std::string DebugString() const;

 private:
  std::string comparator_;
  uint64_t log_number_ = 0;
  uint64_t next_file_number_ = 0;
  SequenceNumber last_sequence_ = 0;
  std::vector<std::pair<int, std::string>> compact_pointers_;
  std::vector<std::pair<int, uint64_t>> deleted_files_;
  std::vector<std::pair<int, FileMetaData>> new_files_;

  bool has_comparator_ = false;
  bool has_log_number_ = false;
  bool has_next_file_number_ = false;
  bool has_last_sequence_ = false;
};

}  // namespace strata

#endif  // STRATA_DB_VERSION_EDIT_H_

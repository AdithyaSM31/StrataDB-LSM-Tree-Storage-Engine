#include "db/version_edit.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "db/dbformat.h"
#include "strata/status.h"
#include "util/check.h"
#include "util/coding.h"

namespace strata {

namespace {

enum Tag : uint8_t {
  kComparator = 1,
  kLogNumber = 2,
  kNextFileNumber = 3,
  kLastSequence = 4,
  kDeletedFile = 5,
  kNewFile = 6,
  kCompactPointer = 7,
};

}  // namespace

void VersionEdit::Clear() {
  comparator_.clear();
  log_number_ = 0;
  next_file_number_ = 0;
  last_sequence_ = 0;
  compact_pointers_.clear();
  deleted_files_.clear();
  new_files_.clear();
  has_comparator_ = false;
  has_log_number_ = false;
  has_next_file_number_ = false;
  has_last_sequence_ = false;
}

void VersionEdit::SetCompactPointer(int level, std::string_view key) {
  STRATA_DCHECK(level >= 0 && level < kNumLevels);
  compact_pointers_.emplace_back(level, std::string(key));
}

void VersionEdit::SetComparatorName(std::string_view name) {
  has_comparator_ = true;
  comparator_ = std::string(name);
}

void VersionEdit::SetLogNumber(uint64_t n) {
  has_log_number_ = true;
  log_number_ = n;
}

void VersionEdit::SetNextFile(uint64_t n) {
  has_next_file_number_ = true;
  next_file_number_ = n;
}

void VersionEdit::SetLastSequence(SequenceNumber s) {
  has_last_sequence_ = true;
  last_sequence_ = s;
}

void VersionEdit::AddFile(int level, uint64_t number, uint64_t size, std::string_view smallest,
                          std::string_view largest) {
  STRATA_DCHECK(level >= 0 && level < kNumLevels);
  FileMetaData f;
  f.number = number;
  f.file_size = size;
  f.smallest = std::string(smallest);
  f.largest = std::string(largest);
  new_files_.emplace_back(level, std::move(f));
}

void VersionEdit::RemoveFile(int level, uint64_t number) {
  STRATA_DCHECK(level >= 0 && level < kNumLevels);
  deleted_files_.emplace_back(level, number);
}

void VersionEdit::EncodeTo(std::string* dst) const {
  if (has_comparator_) {
    PutVarint32(dst, kComparator);
    PutLengthPrefixed(dst, comparator_);
  }
  if (has_log_number_) {
    PutVarint32(dst, kLogNumber);
    PutVarint64(dst, log_number_);
  }
  if (has_next_file_number_) {
    PutVarint32(dst, kNextFileNumber);
    PutVarint64(dst, next_file_number_);
  }
  if (has_last_sequence_) {
    PutVarint32(dst, kLastSequence);
    PutVarint64(dst, last_sequence_);
  }
  for (const auto& [level, number] : deleted_files_) {
    PutVarint32(dst, kDeletedFile);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutVarint64(dst, number);
  }
  for (const auto& [level, key] : compact_pointers_) {
    PutVarint32(dst, kCompactPointer);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutLengthPrefixed(dst, key);
  }
  for (const auto& [level, f] : new_files_) {
    PutVarint32(dst, kNewFile);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutVarint64(dst, f.number);
    PutVarint64(dst, f.file_size);
    PutLengthPrefixed(dst, f.smallest);
    PutLengthPrefixed(dst, f.largest);
  }
}

Status VersionEdit::DecodeFrom(std::string_view src) {
  Clear();
  std::string_view input = src;
  while (!input.empty()) {
    uint32_t tag = 0;
    if (!GetVarint32(&input, &tag)) {
      return Status::Corruption("VersionEdit: truncated tag");
    }
    switch (tag) {
      case kComparator: {
        std::string_view s;
        if (!GetLengthPrefixed(&input, &s)) {
          return Status::Corruption("VersionEdit: truncated comparator name");
        }
        comparator_ = std::string(s);
        has_comparator_ = true;
        break;
      }
      case kLogNumber: {
        uint64_t v = 0;
        if (!GetVarint64(&input, &v)) {
          return Status::Corruption("VersionEdit: truncated log number");
        }
        log_number_ = v;
        has_log_number_ = true;
        break;
      }
      case kNextFileNumber: {
        uint64_t v = 0;
        if (!GetVarint64(&input, &v)) {
          return Status::Corruption("VersionEdit: truncated next file number");
        }
        next_file_number_ = v;
        has_next_file_number_ = true;
        break;
      }
      case kLastSequence: {
        uint64_t v = 0;
        if (!GetVarint64(&input, &v)) {
          return Status::Corruption("VersionEdit: truncated last sequence");
        }
        if (v > kMaxSequenceNumber) {
          return Status::Corruption("VersionEdit: last sequence exceeds 56 bits");
        }
        last_sequence_ = v;
        has_last_sequence_ = true;
        break;
      }
      case kDeletedFile: {
        uint32_t level = 0;
        uint64_t number = 0;
        if (!GetVarint32(&input, &level) || !GetVarint64(&input, &number)) {
          return Status::Corruption("VersionEdit: truncated deleted file");
        }
        if (level >= static_cast<uint32_t>(kNumLevels)) {
          return Status::Corruption("VersionEdit: level >= kNumLevels");
        }
        deleted_files_.emplace_back(static_cast<int>(level), number);
        break;
      }
      case kNewFile: {
        uint32_t level = 0;
        uint64_t number = 0;
        uint64_t file_size = 0;
        std::string_view smallest;
        std::string_view largest;
        if (!GetVarint32(&input, &level) || !GetVarint64(&input, &number) ||
            !GetVarint64(&input, &file_size) || !GetLengthPrefixed(&input, &smallest) ||
            !GetLengthPrefixed(&input, &largest)) {
          return Status::Corruption("VersionEdit: truncated new file");
        }
        if (level >= static_cast<uint32_t>(kNumLevels)) {
          return Status::Corruption("VersionEdit: level >= kNumLevels");
        }
        ParsedInternalKey parsed_key;
        if (!ParseInternalKey(smallest, &parsed_key)) {
          return Status::Corruption("VersionEdit: invalid smallest internal key");
        }
        if (!ParseInternalKey(largest, &parsed_key)) {
          return Status::Corruption("VersionEdit: invalid largest internal key");
        }
        FileMetaData f;
        f.number = number;
        f.file_size = file_size;
        f.smallest = std::string(smallest);
        f.largest = std::string(largest);
        new_files_.emplace_back(static_cast<int>(level), std::move(f));
        break;
      }
      case kCompactPointer: {
        uint32_t level = 0;
        std::string_view key;
        if (!GetVarint32(&input, &level) || !GetLengthPrefixed(&input, &key)) {
          return Status::Corruption("VersionEdit: truncated compact pointer");
        }
        if (level >= static_cast<uint32_t>(kNumLevels)) {
          return Status::Corruption("VersionEdit: level >= kNumLevels in compact pointer");
        }
        ParsedInternalKey parsed_key;
        if (!ParseInternalKey(key, &parsed_key)) {
          return Status::Corruption("VersionEdit: invalid compact pointer internal key");
        }
        compact_pointers_.emplace_back(static_cast<int>(level), std::string(key));
        break;
      }
      default:
        return Status::Corruption("VersionEdit: unknown tag " + std::to_string(tag));
    }
  }
  return Status::OK();
}

std::string VersionEdit::DebugString() const {
  std::ostringstream ss;
  ss << "VersionEdit {";
  if (has_comparator_) {
    ss << " comparator: " << comparator_;
  }
  if (has_log_number_) {
    ss << " log_number: " << log_number_;
  }
  if (has_next_file_number_) {
    ss << " next_file: " << next_file_number_;
  }
  if (has_last_sequence_) {
    ss << " last_seq: " << last_sequence_;
  }
  for (const auto& [level, key] : compact_pointers_) {
    ss << " ptr(L" << level << ", " << key << ")";
  }
  for (const auto& [level, num] : deleted_files_) {
    ss << " del(L" << level << ", " << num << ")";
  }
  for (const auto& [level, f] : new_files_) {
    ss << " add(L" << level << ", " << f.number << ", " << f.file_size << ")";
  }
  ss << " }";
  return ss.str();
}

}  // namespace strata

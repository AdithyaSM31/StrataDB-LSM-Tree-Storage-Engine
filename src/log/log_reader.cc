#include "log/log_reader.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "log/log_format.h"
#include "strata/status.h"
#include "util/check.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace strata::log {

namespace {
enum {
  kEof = kMaxRecordType + 1,
  kBadRecord = kMaxRecordType + 2,
};
}  // namespace

Reader::Reader(SequentialFile* file, Reporter* reporter, bool checksum)
    : file_(file),
      reporter_(reporter),
      checksum_(checksum),
      backing_store_(std::make_unique<char[]>(kBlockSize)),
      buffer_(),
      eof_(false),
      in_fragmented_record_(false) {
  STRATA_DCHECK(file_ != nullptr);
}

void Reader::ReportCorruption(size_t bytes, const Status& status) {
  if (reporter_ != nullptr) {
    reporter_->Corruption(bytes, status);
  }
}

int Reader::ReadPhysicalRecord(std::string_view* result) {
  STRATA_DCHECK(result != nullptr);
  while (true) {
    if (buffer_.size() < kHeaderSize) {
      if (!eof_) {
        buffer_ = {};
        Status status = file_->Read(kBlockSize, &buffer_, backing_store_.get());
        if (!status.ok()) {
          buffer_ = {};
          ReportCorruption(kBlockSize, status);
          eof_ = true;
          return kEof;
        }
        if (buffer_.size() < kBlockSize) {
          eof_ = true;
        }
        continue;
      } else {
        buffer_ = {};
        return kEof;  // half a header at the end: a TORN write
      }
    }

    const auto* header = reinterpret_cast<const uint8_t*>(buffer_.data());
    const uint32_t length =
        static_cast<uint32_t>(header[4]) | (static_cast<uint32_t>(header[5]) << 8);
    const uint8_t type = header[6];

    if (kHeaderSize + length > buffer_.size()) {
      const size_t drop = buffer_.size();
      buffer_ = {};
      if (!eof_) {
        ReportCorruption(drop, Status::Corruption("bad record length"));
        return kBadRecord;
      }
      return kEof;  // the writer died mid-record: TORN, silent
    }

    if (type == kZeroType && length == 0) {
      buffer_ = {};
      return kBadRecord;  // skip it, report nothing
    }

    if (checksum_) {
      const uint32_t expected_crc = crc32c::Unmask(DecodeFixed32(buffer_.data()));
      const uint32_t actual_crc = crc32c::Value(buffer_.data() + 6, 1 + length);
      if (expected_crc != actual_crc) {
        const size_t drop = buffer_.size();
        buffer_ = {};
        ReportCorruption(drop, Status::Corruption("checksum mismatch"));
        return kBadRecord;
      }
    }

    *result = std::string_view(buffer_.data() + kHeaderSize, length);
    buffer_.remove_prefix(kHeaderSize + length);
    return type;
  }
}

bool Reader::ReadRecord(std::string_view* record, std::string* scratch) {
  STRATA_DCHECK(record != nullptr);
  STRATA_DCHECK(scratch != nullptr);
  scratch->clear();
  std::string_view fragment;

  while (true) {
    const int record_type = ReadPhysicalRecord(&fragment);

    switch (record_type) {
      case kFullType:
        if (in_fragmented_record_) {
          ReportCorruption(scratch->size(), Status::Corruption("partial record without end"));
          scratch->clear();
          in_fragmented_record_ = false;
        }
        *record = fragment;
        return true;

      case kFirstType:
        if (in_fragmented_record_) {
          ReportCorruption(scratch->size(), Status::Corruption("partial record without end"));
          scratch->clear();
        }
        scratch->assign(fragment.data(), fragment.size());
        in_fragmented_record_ = true;
        break;

      case kMiddleType:
        if (!in_fragmented_record_) {
          ReportCorruption(fragment.size(),
                           Status::Corruption("missing start of fragmented record"));
        } else {
          scratch->append(fragment.data(), fragment.size());
        }
        break;

      case kLastType:
        if (!in_fragmented_record_) {
          ReportCorruption(fragment.size(),
                           Status::Corruption("missing start of fragmented record"));
        } else {
          scratch->append(fragment.data(), fragment.size());
          *record = *scratch;
          in_fragmented_record_ = false;
          return true;
        }
        break;

      case kEof:
        if (in_fragmented_record_) {
          scratch->clear();
          in_fragmented_record_ = false;
        }
        return false;

      case kBadRecord:
        if (in_fragmented_record_) {
          ReportCorruption(scratch->size(), Status::Corruption("error in middle of record"));
          scratch->clear();
          in_fragmented_record_ = false;
        }
        break;

      default: {
        size_t drop = fragment.size();
        if (in_fragmented_record_) {
          drop += scratch->size();
          scratch->clear();
          in_fragmented_record_ = false;
        }
        ReportCorruption(drop, Status::Corruption("unknown record type"));
        break;
      }
    }
  }
}

}  // namespace strata::log

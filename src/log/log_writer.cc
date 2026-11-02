#include "log/log_writer.h"

#include <algorithm>
#include <cstdint>

#include "log/log_format.h"
#include "util/check.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace strata::log {

static void InitTypeCrc(uint32_t type_crc[kMaxRecordType + 1]) {
  for (int i = 0; i <= kMaxRecordType; ++i) {
    const char c = static_cast<char>(i);
    type_crc[i] = crc32c::Value(&c, 1);
  }
}

Writer::Writer(WritableFile* dest) : dest_(dest), block_offset_(0) {
  STRATA_DCHECK(dest_ != nullptr);
  InitTypeCrc(type_crc_);
}

Writer::Writer(WritableFile* dest, uint64_t dest_length)
    : dest_(dest), block_offset_(static_cast<int>(dest_length % kBlockSize)) {
  STRATA_DCHECK(dest_ != nullptr);
  InitTypeCrc(type_crc_);
}

Status Writer::AddRecord(std::string_view slice) {
  const char* ptr = (slice.data() != nullptr) ? slice.data() : "";
  size_t left = slice.size();
  bool begin = true;
  Status s;

  do {
    const int leftover = static_cast<int>(kBlockSize) - block_offset_;
    STRATA_DCHECK(leftover >= 0);
    if (leftover < static_cast<int>(kHeaderSize)) {
      if (leftover > 0) {
        static_assert(kHeaderSize <= 7);
        static const char kZeroTrailer[kHeaderSize] = {0};
        s = dest_->Append(std::string_view(kZeroTrailer, static_cast<size_t>(leftover)));
        if (!s.ok()) {
          return s;
        }
      }
      block_offset_ = 0;
    }

    const size_t avail = kBlockSize - block_offset_ - kHeaderSize;
    const size_t fragment = std::min(left, avail);
    const bool end = (left == fragment);

    RecordType type;
    if (begin && end) {
      type = kFullType;
    } else if (begin) {
      type = kFirstType;
    } else if (end) {
      type = kLastType;
    } else {
      type = kMiddleType;
    }

    s = EmitPhysicalRecord(type, ptr, fragment);
    ptr += fragment;
    left -= fragment;
    begin = false;
  } while (s.ok() && left > 0);

  return s;
}

Status Writer::EmitPhysicalRecord(RecordType type, const char* ptr, size_t length) {
  STRATA_DCHECK(length <= 0xffff);
  STRATA_DCHECK(block_offset_ + kHeaderSize + length <= kBlockSize);
  const char* safe_ptr = (ptr != nullptr) ? ptr : "";

  char buf[kHeaderSize];
  buf[4] = static_cast<char>(length & 0xff);
  buf[5] = static_cast<char>((length >> 8) & 0xff);
  buf[6] = static_cast<char>(type);

  uint32_t crc = crc32c::Extend(type_crc_[type], safe_ptr, length);
  crc = crc32c::Mask(crc);
  EncodeFixed32(buf, crc);

  Status s = dest_->Append(std::string_view(buf, kHeaderSize));
  if (s.ok()) {
    s = dest_->Append(std::string_view(safe_ptr, length));
    if (s.ok()) {
      s = dest_->Flush();
    }
  }
  block_offset_ += static_cast<int>(kHeaderSize + length);
  return s;
}

}  // namespace strata::log

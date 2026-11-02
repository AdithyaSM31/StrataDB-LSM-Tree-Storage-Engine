#ifndef STRATA_LOG_LOG_WRITER_H_
#define STRATA_LOG_LOG_WRITER_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "log/log_format.h"
#include "strata/env.h"
#include "strata/status.h"

namespace strata::log {

class Writer {
 public:
  explicit Writer(WritableFile* dest);               // new, empty file
  Writer(WritableFile* dest, uint64_t dest_length);  // appending to an existing file
  ~Writer() = default;

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;
  Writer(Writer&&) = delete;
  Writer& operator=(Writer&&) = delete;

  Status AddRecord(std::string_view slice);

 private:
  Status EmitPhysicalRecord(RecordType type, const char* ptr, size_t length);

  WritableFile* dest_;  // not owned
  int block_offset_;    // position within the current block
  uint32_t type_crc_[kMaxRecordType + 1];
};

}  // namespace strata::log

#endif  // STRATA_LOG_LOG_WRITER_H_

#ifndef STRATA_LOG_LOG_READER_H_
#define STRATA_LOG_LOG_READER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "log/log_format.h"
#include "strata/env.h"
#include "strata/status.h"

namespace strata::log {

class Reader {
 public:
  class Reporter {
   public:
    virtual ~Reporter() = default;
    // `bytes` were dropped because of `status`. The reader keeps going.
    virtual void Corruption(size_t bytes, const Status& status) = 0;
  };

  Reader(SequentialFile* file, Reporter* reporter, bool checksum);
  ~Reader() = default;

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;
  Reader(Reader&&) = delete;
  Reader& operator=(Reader&&) = delete;

  // true and a record, or false at the end of the log.
  // *record may point into scratch or into the reader's buffer: valid only until the next call.
  bool ReadRecord(std::string_view* record, std::string* scratch);

 private:
  void ReportCorruption(size_t bytes, const Status& status);
  int ReadPhysicalRecord(std::string_view* result);  // a RecordType, kEof or kBadRecord

  SequentialFile* file_;
  Reporter* reporter_;
  bool checksum_;
  std::unique_ptr<char[]> backing_store_;  // one block
  std::string_view buffer_;                // unread part of the current block
  bool eof_ = false;                       // the last file read returned less than a full block
  bool in_fragmented_record_ = false;
};

}  // namespace strata::log

#endif  // STRATA_LOG_LOG_READER_H_

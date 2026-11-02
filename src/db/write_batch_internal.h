#ifndef STRATA_DB_WRITE_BATCH_INTERNAL_H_
#define STRATA_DB_WRITE_BATCH_INTERNAL_H_

#include <cstdint>
#include <string_view>

#include "db/dbformat.h"
#include "strata/status.h"
#include "strata/write_batch.h"

namespace strata {

class MemTable;

class WriteBatchInternal {
 public:
  static uint32_t Count(const WriteBatch* b);
  static void SetCount(WriteBatch* b, uint32_t count);
  static SequenceNumber Sequence(const WriteBatch* b);
  static void SetSequence(WriteBatch* b, SequenceNumber seq);
  static std::string_view Contents(const WriteBatch* b);  // what the log writer appends
  static void SetContents(WriteBatch* b,
                          std::string_view contents);  // what recovery calls with a WAL record
  static Status InsertInto(const WriteBatch* b, MemTable* mem);
  static void Append(WriteBatch* dst, const WriteBatch* src);
};

}  // namespace strata

#endif  // STRATA_DB_WRITE_BATCH_INTERNAL_H_

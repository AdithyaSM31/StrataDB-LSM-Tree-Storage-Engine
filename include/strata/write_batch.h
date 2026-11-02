#ifndef STRATA_INCLUDE_WRITE_BATCH_H_
#define STRATA_INCLUDE_WRITE_BATCH_H_

#include <cstddef>
#include <string>
#include <string_view>

#include "strata/status.h"

namespace strata {

class MemTable;
class WriteBatchInternal;

class WriteBatch {
 public:
  WriteBatch();  // rep_ = 12 zero bytes
  ~WriteBatch() = default;

  WriteBatch(const WriteBatch&) = default;
  WriteBatch& operator=(const WriteBatch&) = default;
  WriteBatch(WriteBatch&&) noexcept = default;
  WriteBatch& operator=(WriteBatch&&) noexcept = default;

  void Put(std::string_view key, std::string_view value);
  void Delete(std::string_view key);
  void Clear();                           // back to 12 zero bytes
  size_t ApproximateSize() const;         // rep_.size()
  void Append(const WriteBatch& source);  // used by group commit

  class Handler {
   public:
    virtual ~Handler() = default;
    virtual void Put(std::string_view key, std::string_view value) = 0;
    virtual void Delete(std::string_view key) = 0;
  };
  Status Iterate(Handler* handler) const;

 private:
  friend class WriteBatchInternal;
  std::string rep_;
};

}  // namespace strata

#endif  // STRATA_INCLUDE_WRITE_BATCH_H_

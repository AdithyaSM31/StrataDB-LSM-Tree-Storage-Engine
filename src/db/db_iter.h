#ifndef STRATA_DB_DB_ITER_H_
#define STRATA_DB_DB_ITER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "db/dbformat.h"
#include "strata/iterator.h"
#include "util/comparator.h"

namespace strata {

class DBIter : public Iterator {
 public:
  DBIter(const Comparator* cmp, std::unique_ptr<Iterator> iter, SequenceNumber sequence);
  ~DBIter() override;

  DBIter(const DBIter&) = delete;
  DBIter& operator=(const DBIter&) = delete;

  bool Valid() const override;
  std::string_view key() const override;
  std::string_view value() const override;
  Status status() const override;

  void SeekToFirst() override;
  void SeekToLast() override;
  void Seek(std::string_view target) override;
  void Next() override;
  void Prev() override;

  void RegisterCleanup(std::function<void()> cleanup);

 private:
  enum Direction : uint8_t {
    kForward,
    kReverse,
  };

  void FindNextUserEntry(bool skipping, std::string* skip);
  void FindPrevUserEntry();
  bool ParseKey(ParsedInternalKey* ikey);
  void ClearSavedValue() { saved_value_.clear(); }

  const Comparator* const user_comparator_;
  std::unique_ptr<Iterator> iter_;
  SequenceNumber sequence_;

  Direction direction_ = kForward;
  bool valid_ = false;
  Status status_;
  std::string saved_key_;
  std::string saved_value_;

  std::vector<std::function<void()>> cleanups_;
};

// Return a new iterator that converts internal keys (from an internal merging iterator)
// into user keys, filtering by snapshot sequence and skipping deleted keys.
std::unique_ptr<DBIter> NewDBIterator(const Comparator* cmp, std::unique_ptr<Iterator> iter,
                                      SequenceNumber sequence);

}  // namespace strata

#endif  // STRATA_DB_DB_ITER_H_

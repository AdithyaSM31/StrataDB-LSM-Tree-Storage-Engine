#ifndef STRATA_MEMTABLE_MEMTABLE_H_
#define STRATA_MEMTABLE_MEMTABLE_H_

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "db/dbformat.h"
#include "memtable/skiplist.h"
#include "strata/iterator.h"
#include "strata/status.h"
#include "util/arena.h"

namespace strata {

class MemTable {
 public:
  struct KeyComparator {
    const InternalKeyComparator comparator;
    explicit KeyComparator(const InternalKeyComparator& c) : comparator(c) {}
    int operator()(const char* a, const char* b) const;
  };

  explicit MemTable(const InternalKeyComparator& cmp);
  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  void Ref();
  void Unref();  // deletes itself when the count reaches 0

  size_t ApproximateMemoryUsage();  // arena_.MemoryUsage()

  // REQUIRES: one writer at a time (the write leader).
  void Add(SequenceNumber seq, ValueType type, std::string_view key, std::string_view value);

  // true  -> this memtable has the answer: a value (*value set) or a tombstone (*s = NotFound)
  // false -> no entry for this user key here; the caller checks the next layer
  bool Get(const LookupKey& key, std::string* value, Status* s);

  std::unique_ptr<Iterator> NewIterator();  // yields internal keys and values

 private:
  ~MemTable();  // private: only Unref() may delete

  friend class MemTableIterator;

  KeyComparator comparator_;
  int refs_;  // REQUIRES: DB mutex held
  Arena arena_;
  SkipList<const char*, KeyComparator> table_;
};

}  // namespace strata

#endif  // STRATA_MEMTABLE_MEMTABLE_H_

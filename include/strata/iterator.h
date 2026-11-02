#ifndef STRATA_INCLUDE_ITERATOR_H_
#define STRATA_INCLUDE_ITERATOR_H_

#include <memory>
#include <string_view>

#include "strata/status.h"

namespace strata {

// An Iterator yields a sequence of key/value pairs from a source.
// Thread safety: an iterator is not thread-safe; distinct threads must use
// distinct iterators, or synchronize externally.
class Iterator {
 public:
  Iterator() = default;
  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;
  virtual ~Iterator() = default;

  // An iterator is either positioned at a key/value pair, or not valid.
  virtual bool Valid() const = 0;

  // Position at the first key in the source. The iterator is Valid()
  // after this call iff the source is not empty.
  virtual void SeekToFirst() = 0;

  // Position at the last key in the source. The iterator is Valid()
  // after this call iff the source is not empty.
  virtual void SeekToLast() = 0;

  // Position at the first key in the source that is at or past target.
  // The iterator is Valid() after this call iff the source contains
  // an entry that comes at or past target.
  virtual void Seek(std::string_view target) = 0;

  // Moves to the next entry in the source. After this call, Valid() is
  // true iff the iterator was not already at the last entry in the source.
  // REQUIRES: Valid()
  virtual void Next() = 0;

  // Moves to the previous entry in the source. After this call, Valid() is
  // true iff the iterator was not already at the first entry in the source.
  // REQUIRES: Valid()
  virtual void Prev() = 0;

  // Return the key for the current entry. The underlying storage for
  // the returned string_view is valid only until the next modification of
  // the iterator.
  // REQUIRES: Valid()
  virtual std::string_view key() const = 0;

  // Return the value for the current entry. The underlying storage for
  // the returned string_view is valid only until the next modification of
  // the iterator.
  // REQUIRES: Valid()
  virtual std::string_view value() const = 0;

  // If an error has occurred, return it. Else return an ok status.
  virtual Status status() const = 0;
};

// Returns an iterator over nothing: never Valid(), and status() is OK.
std::unique_ptr<Iterator> NewEmptyIterator();

// Returns an iterator over nothing whose status() is `status`. Used to report
// an error (or an unsupported operation) through the Iterator interface.
std::unique_ptr<Iterator> NewErrorIterator(const Status& status);

}  // namespace strata

#endif  // STRATA_INCLUDE_ITERATOR_H_

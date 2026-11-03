#ifndef STRATA_INCLUDE_DB_H_
#define STRATA_INCLUDE_DB_H_

#include <memory>
#include <string>
#include <string_view>

#include "strata/iterator.h"
#include "strata/options.h"
#include "strata/status.h"
#include "strata/write_batch.h"

namespace strata {

// An abstract handle to a particular state of a DB.
// A Snapshot is an immutable object and can therefore be safely
// accessed from multiple threads without any external synchronization.
class Snapshot {
 public:
  virtual ~Snapshot() = default;
};

// A DB is a persistent ordered map from keys to values.
// A DB is safe for concurrent access from multiple threads without
// any external synchronization.
class DB {
 public:
  // Open the database with the specified "name".
  // Stores a pointer to a heap-allocated database in *out and returns
  // OK on success.
  // Stores nullptr in *out and returns a non-OK status on error.
  // Caller must delete *out when it is no longer in use.
  static Status Open(const Options& options, const std::string& name, std::unique_ptr<DB>* out);

  DB() = default;
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;
  virtual ~DB() = default;

  // Set the database entry for "key" to "value".
  Status Put(const WriteOptions& opts, std::string_view key, std::string_view value) {
    WriteBatch batch;
    batch.Put(key, value);
    return Write(opts, &batch);
  }

  // Remove the database entry for "key".
  Status Delete(const WriteOptions& opts, std::string_view key) {
    WriteBatch batch;
    batch.Delete(key);
    return Write(opts, &batch);
  }

  // Apply the specified updates to the database.
  virtual Status Write(const WriteOptions& opts, WriteBatch* batch) = 0;

  // If the database contains an entry for "key" store the
  // corresponding value in *value and return OK.
  //
  // If there is no entry for "key" leave *value unchanged and return
  // a status for which Status::IsNotFound() returns true.
  virtual Status Get(const ReadOptions& opts, std::string_view key, std::string* value) = 0;

  // Return a heap-allocated iterator over the contents of the database.
  // The result of NewIterator() is initially invalid (caller must
  // call one of the Seek methods on the iterator before using it).
  //
  // Caller should delete the iterator when it is no longer needed.
  virtual std::unique_ptr<Iterator> NewIterator(const ReadOptions& opts) = 0;

  // Return a handle to the current DB state. Iterators and Gets created with
  // this handle will all observe a stable snapshot of the current DB state.
  // The caller must call ReleaseSnapshot(result) when the snapshot is no
  // longer needed, and must release every snapshot before the DB is
  // destroyed: the DB frees any it still holds, so a later ReleaseSnapshot
  // would free it twice.
  virtual const Snapshot* GetSnapshot() = 0;

  // Release a previously acquired snapshot. The caller must not
  // use "snapshot" after this call.
  virtual void ReleaseSnapshot(const Snapshot* snapshot) = 0;

  // Force a flush of the current memtable to an L0 SSTable.
  virtual Status Flush() = 0;

  // Compact the underlying storage for the user-key range [begin, end], pushing
  // it down to the deepest level that holds data in that range. Overwritten
  // values and deleted keys that no snapshot needs are dropped. An empty begin
  // means "from the first key" and an empty end means "to the last key", so
  // CompactRange("", "") compacts the whole database.
  virtual Status CompactRange(std::string_view begin, std::string_view end) = 0;

  // DB implementations can export properties about their state
  // via this method. If "name" is a valid property understood by this
  // DB implementation, fills "*value" with its current value and returns
  // true. Otherwise returns false.
  virtual bool GetProperty(std::string_view name, std::string* value) = 0;
};

}  // namespace strata

#endif  // STRATA_INCLUDE_DB_H_

#ifndef STRATA_TESTS_CRASH_MODEL_H_
#define STRATA_TESTS_CRASH_MODEL_H_

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>

#include "strata/db.h"
#include "strata/iterator.h"
#include "strata/options.h"
#include "strata/status.h"

namespace strata {

// A fixed stream of writes, numbered from 1, that crash tests replay. Keys come
// from a small space, so files overlap and compactions really merge.
struct CrashWorkload {
  uint64_t key_space = 200;
  size_t value_size = 100;

  std::string Key(uint64_t seq) const {
    char buf[32];
    // Knuth's multiplicative hash: scatters the keys, yet repeats them often.
    std::snprintf(buf, sizeof(buf), "k_%06" PRIu64, (seq * 2654435761U) % key_space);
    return buf;
  }

  // Every 11th write is a delete.
  bool IsDelete(uint64_t seq) const { return seq % 11 == 0; }

  std::string Value(uint64_t seq) const {
    std::string value = "seq=" + std::to_string(seq) + ";";
    if (value.size() < value_size) {
      value.resize(value_size, 'x');
    }
    return value;
  }

  Status Apply(DB* db, uint64_t seq, bool sync) const {
    WriteOptions opts;
    opts.sync = sync;
    return IsDelete(seq) ? db->Delete(opts, Key(seq)) : db->Put(opts, Key(seq), Value(seq));
  }
};

// Checks the crash guarantee: what a DB holds after a crash is exactly the
// result of writes 1..R, with no gaps, for some R at or past the last write
// known to be durable.
class CrashModel {
 public:
  explicit CrashModel(CrashWorkload workload) : workload_(workload) {}

  // R from the last successful Verify(), and so where the next writes continue.
  uint64_t applied() const { return applied_; }

  // Finds R, then compares the whole DB against writes 1..R. `durable` is the
  // last write the DB confirmed as durable.
  Status Verify(DB* db, uint64_t durable) {
    while (applied_ < durable) {
      Step();
    }
    while (true) {
      const uint64_t next = applied_ + 1;
      std::string value;
      Status s = db->Get(ReadOptions(), workload_.Key(next), &value);
      if (!s.ok() && !s.IsNotFound()) {
        return s;
      }
      // A delete of a key that was already absent shows no effect either way,
      // and counting it as applied gives the same state, so that is safe.
      const bool visible =
          workload_.IsDelete(next) ? s.IsNotFound() : (s.ok() && value == workload_.Value(next));
      if (!visible) {
        break;
      }
      Step();
    }

    std::unique_ptr<Iterator> it = db->NewIterator(ReadOptions());
    auto expected = state_.begin();
    for (it->SeekToFirst(); it->Valid(); it->Next(), ++expected) {
      if (expected == state_.end() || it->key() != expected->first ||
          it->value() != workload_.Value(expected->second)) {
        return Status::Corruption("DB differs from writes 1.." + std::to_string(applied_) +
                                  " at key " + std::string(it->key()));
      }
    }
    if (!it->status().ok()) {
      return it->status();
    }
    if (expected != state_.end()) {
      return Status::Corruption("DB differs from writes 1.." + std::to_string(applied_) +
                                ": missing key " + expected->first);
    }
    return Status::OK();
  }

 private:
  void Step() {
    ++applied_;
    if (workload_.IsDelete(applied_)) {
      state_.erase(workload_.Key(applied_));
    } else {
      state_[workload_.Key(applied_)] = applied_;
    }
  }

  CrashWorkload workload_;
  uint64_t applied_ = 0;
  std::map<std::string, uint64_t> state_;  // key -> the write that last set it
};

}  // namespace strata

#endif  // STRATA_TESTS_CRASH_MODEL_H_

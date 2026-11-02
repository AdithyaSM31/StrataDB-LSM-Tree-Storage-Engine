#include "table/block.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "strata/status.h"
#include "util/check.h"
#include "util/coding.h"

namespace strata {

Block::Block(std::string contents) : data_(std::move(contents)) {
  // Need at least the restart count, and offsets must fit in 32 bits.
  if (data_.size() < sizeof(uint32_t) || data_.size() > UINT32_MAX) {
    malformed_ = true;
    return;
  }
  num_restarts_ = DecodeFixed32(data_.data() + data_.size() - sizeof(uint32_t));
  // Bound the count by the space available, computed so it cannot overflow.
  // (data_.size() - 4 * (num_restarts + 1) could wrap around for a huge count.)
  const size_t max_restarts = (data_.size() - sizeof(uint32_t)) / sizeof(uint32_t);
  // BlockBuilder always writes at least one restart point (offset 0), so a
  // count of zero means corruption, not an empty block.
  if (num_restarts_ == 0 || num_restarts_ > max_restarts) {
    malformed_ = true;
    return;
  }
  restart_offset_ =
      static_cast<uint32_t>(data_.size() - (1 + size_t{num_restarts_}) * sizeof(uint32_t));
}

namespace {

// Decodes the three lengths at the start of an entry. Returns a pointer to the
// key suffix, or nullptr if the entry is malformed or runs past `limit`.
const char* DecodeEntry(const char* p, const char* limit, uint32_t* shared, uint32_t* non_shared,
                        uint32_t* value_length) {
  if (limit - p < 3) {
    return nullptr;
  }
  *shared = static_cast<uint8_t>(p[0]);
  *non_shared = static_cast<uint8_t>(p[1]);
  *value_length = static_cast<uint8_t>(p[2]);
  if ((*shared | *non_shared | *value_length) < 128) {
    // Fast path: each length fits in a single byte.
    p += 3;
  } else {
    p = GetVarint32Ptr(p, limit, shared);
    if (p == nullptr) {
      return nullptr;
    }
    p = GetVarint32Ptr(p, limit, non_shared);
    if (p == nullptr) {
      return nullptr;
    }
    p = GetVarint32Ptr(p, limit, value_length);
    if (p == nullptr) {
      return nullptr;
    }
  }
  // Summed in 64 bits: two 32-bit lengths can overflow a 32-bit sum.
  const uint64_t needed = uint64_t{*non_shared} + uint64_t{*value_length};
  if (static_cast<uint64_t>(limit - p) < needed) {
    return nullptr;
  }
  return p;
}

}  // namespace

class Block::Iter : public Iterator {
 public:
  Iter(const Comparator* comparator, const char* data, uint32_t restarts, uint32_t num_restarts)
      : comparator_(comparator),
        data_(data),
        restarts_(restarts),
        num_restarts_(num_restarts),
        current_(restarts),
        restart_index_(num_restarts),
        next_(restarts) {
    STRATA_DCHECK(num_restarts_ > 0);
  }

  bool Valid() const override { return current_ < restarts_; }
  Status status() const override { return status_; }

  std::string_view key() const override {
    STRATA_DCHECK(Valid());
    return key_;
  }

  std::string_view value() const override {
    STRATA_DCHECK(Valid());
    return value_;
  }

  void Next() override {
    STRATA_DCHECK(Valid());
    ParseNextKey();
  }

  void Prev() override {
    STRATA_DCHECK(Valid());
    // Entries have no backward links. Back up to the last restart point that
    // starts before the current entry, then scan forward to the entry just
    // before it. This costs up to restart_interval entries per step.
    const uint32_t original = current_;
    while (GetRestartPoint(restart_index_) >= original) {
      if (restart_index_ == 0) {
        // No entry before the first one.
        MarkInvalid();
        return;
      }
      --restart_index_;
    }
    if (!SeekToRestartPoint(restart_index_)) {
      return;
    }
    // Each step moves strictly forward, and we stop at the entry whose
    // successor is at or past `original`, so this always terminates.
    while (ParseNextKey() && NextEntryOffset() < original) {
    }
  }

  void Seek(std::string_view target) override {
    // Binary search over restart points for the last one whose key is < target.
    uint32_t left = 0;
    uint32_t right = num_restarts_ - 1;
    while (left < right) {
      // Round up, or `left = mid` could make no progress and loop forever.
      const uint32_t mid = left + (right - left + 1) / 2;
      const uint32_t region_offset = GetRestartPoint(mid);
      if (region_offset >= restarts_) {
        CorruptionError();
        return;
      }
      uint32_t shared = 0;
      uint32_t non_shared = 0;
      uint32_t value_length = 0;
      const char* key_ptr = DecodeEntry(data_ + region_offset, data_ + restarts_, &shared,
                                        &non_shared, &value_length);
      // A restart point always stores its key whole.
      if (key_ptr == nullptr || shared != 0) {
        CorruptionError();
        return;
      }
      const std::string_view mid_key(key_ptr, non_shared);
      if (comparator_->Compare(mid_key, target) < 0) {
        left = mid;  // keys before `mid` are all < target
      } else {
        right = mid - 1;  // the answer is before `mid`
      }
    }

    // Scan forward from that restart point to the first key >= target.
    if (!SeekToRestartPoint(left)) {
      return;
    }
    while (ParseNextKey()) {
      if (comparator_->Compare(key_, target) >= 0) {
        return;
      }
    }
  }

  void SeekToFirst() override {
    if (SeekToRestartPoint(0)) {
      ParseNextKey();
    }
  }

  void SeekToLast() override {
    if (!SeekToRestartPoint(num_restarts_ - 1)) {
      return;
    }
    while (ParseNextKey() && NextEntryOffset() < restarts_) {
    }
  }

 private:
  // Offset just past the current entry, where the next entry starts.
  uint32_t NextEntryOffset() const { return next_; }

  uint32_t GetRestartPoint(uint32_t index) const {
    STRATA_DCHECK(index < num_restarts_);
    return DecodeFixed32(data_ + restarts_ + index * sizeof(uint32_t));
  }

  // Positions just before the entry at restart point `index`, so the next
  // ParseNextKey() reads it. Returns false (and reports corruption) if the
  // restart offset points outside the entries.
  bool SeekToRestartPoint(uint32_t index) {
    key_.clear();
    restart_index_ = index;
    const uint32_t offset = GetRestartPoint(index);
    // offset == restarts_ is allowed: an empty block has one restart at 0.
    if (offset > restarts_) {
      CorruptionError();
      return false;
    }
    // ParseNextKey() starts reading at NextEntryOffset().
    value_ = {};
    next_ = offset;
    return true;
  }

  // Reads the entry at NextEntryOffset(). Returns false at the end of the
  // entries or on corruption (status_ is then set).
  bool ParseNextKey() {
    current_ = NextEntryOffset();
    const char* p = data_ + current_;
    const char* const limit = data_ + restarts_;
    if (p >= limit) {
      MarkInvalid();  // no more entries
      return false;
    }

    uint32_t shared = 0;
    uint32_t non_shared = 0;
    uint32_t value_length = 0;
    p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
    // A corrupt `shared` could claim more of the previous key than exists.
    if (p == nullptr || key_.size() < shared) {
      CorruptionError();
      return false;
    }
    // The key is not stored contiguously, so rebuild it in key_.
    key_.resize(shared);
    key_.append(p, non_shared);
    value_ = std::string_view(p + non_shared, value_length);
    next_ = static_cast<uint32_t>((p + non_shared + value_length) - data_);
    while (restart_index_ + 1 < num_restarts_ && GetRestartPoint(restart_index_ + 1) < current_) {
      ++restart_index_;
    }
    return true;
  }

  void MarkInvalid() {
    current_ = restarts_;
    restart_index_ = num_restarts_;
  }

  void CorruptionError() {
    MarkInvalid();
    status_ = Status::Corruption("bad entry in block");
    key_.clear();
    value_ = {};
    next_ = restarts_;
  }

  const Comparator* const comparator_;
  const char* const data_;   // the block's contents
  const uint32_t restarts_;  // offset of the restart array (= end of the entries)
  const uint32_t num_restarts_;

  uint32_t current_;        // offset of the current entry; >= restarts_ means not valid
  uint32_t restart_index_;  // restart point at or before current_
  std::string key_;         // the current key, rebuilt from shared prefixes
  std::string_view value_;  // points into the block
  uint32_t next_;           // offset where the entry after the current one starts
  Status status_;
};

std::unique_ptr<Iterator> Block::NewIterator(const Comparator* comparator) const {
  STRATA_DCHECK(comparator != nullptr);
  if (malformed_) {
    return NewErrorIterator(Status::Corruption("bad block contents"));
  }
  return std::make_unique<Iter>(comparator, data_.data(), restart_offset_, num_restarts_);
}

}  // namespace strata

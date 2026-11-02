#include "table/block_builder.h"

#include <algorithm>
#include <cstdint>
#include <string_view>

#include "util/check.h"
#include "util/coding.h"

namespace strata {

BlockBuilder::BlockBuilder(int restart_interval, const Comparator* comparator)
    : restart_interval_(restart_interval), comparator_(comparator) {
  STRATA_CHECK(restart_interval_ >= 1);
  restarts_.push_back(0);  // the first entry is always a restart point
}

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.clear();
  restarts_.push_back(0);
  counter_ = 0;
  finished_ = false;
  last_key_.clear();
}

size_t BlockBuilder::CurrentSizeEstimate() const {
  return buffer_.size() + restarts_.size() * sizeof(uint32_t) + sizeof(uint32_t);
}

void BlockBuilder::Add(std::string_view key, std::string_view value) {
  STRATA_DCHECK(!finished_);
  STRATA_DCHECK(counter_ <= restart_interval_);
  STRATA_DCHECK(buffer_.empty() || comparator_ == nullptr ||
                comparator_->Compare(key, last_key_) > 0);
  // Offsets and lengths are stored as 32-bit values.
  STRATA_CHECK(key.size() <= UINT32_MAX && value.size() <= UINT32_MAX);

  size_t shared = 0;
  if (counter_ < restart_interval_) {
    // Share as much of the previous key as matches.
    const size_t min_length = std::min(last_key_.size(), key.size());
    while (shared < min_length && last_key_[shared] == key[shared]) {
      ++shared;
    }
  } else {
    // Start a new restart point: store this key whole.
    STRATA_CHECK(buffer_.size() <= UINT32_MAX);
    restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
    counter_ = 0;
  }
  const size_t non_shared = key.size() - shared;

  PutVarint32(&buffer_, static_cast<uint32_t>(shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(non_shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(value.size()));
  // An empty string_view may have a null data(); never pass that on.
  const std::string_view key_suffix = key.substr(shared);
  if (!key_suffix.empty()) {
    buffer_.append(key_suffix.data(), key_suffix.size());
  }
  if (!value.empty()) {
    buffer_.append(value.data(), value.size());
  }

  // Keep last_key_ equal to key, reusing the shared prefix.
  last_key_.resize(shared);
  if (!key_suffix.empty()) {
    last_key_.append(key_suffix.data(), key_suffix.size());
  }
  STRATA_DCHECK(std::string_view(last_key_) == key);
  ++counter_;
}

std::string_view BlockBuilder::Finish() {
  STRATA_DCHECK(!finished_);
  for (const uint32_t restart : restarts_) {
    PutFixed32(&buffer_, restart);
  }
  PutFixed32(&buffer_, static_cast<uint32_t>(restarts_.size()));
  finished_ = true;
  return buffer_;
}

}  // namespace strata

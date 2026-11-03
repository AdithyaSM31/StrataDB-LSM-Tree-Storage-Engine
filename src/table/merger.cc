#include "table/merger.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "strata/iterator.h"
#include "strata/status.h"
#include "util/check.h"
#include "util/comparator.h"

namespace strata {
namespace {

enum Direction : uint8_t {
  kForward,
  kReverse,
};

class MergingIterator : public Iterator {
 public:
  MergingIterator(const Comparator* comparator, std::vector<std::unique_ptr<Iterator>> children)
      : comparator_(comparator), children_(std::move(children)) {}

  ~MergingIterator() override = default;

  bool Valid() const override { return current_ != nullptr && current_->Valid(); }

  std::string_view key() const override {
    STRATA_DCHECK(Valid());
    return current_->key();
  }

  std::string_view value() const override {
    STRATA_DCHECK(Valid());
    return current_->value();
  }

  Status status() const override {
    for (const auto& child : children_) {
      if (child != nullptr) {
        Status s = child->status();
        if (!s.ok()) {
          return s;
        }
      }
    }
    return Status::OK();
  }

  void SeekToFirst() override {
    for (auto& child : children_) {
      if (child != nullptr) {
        child->SeekToFirst();
      }
    }
    FindSmallest();
    direction_ = kForward;
  }

  void SeekToLast() override {
    for (auto& child : children_) {
      if (child != nullptr) {
        child->SeekToLast();
      }
    }
    FindLargest();
    direction_ = kReverse;
  }

  void Seek(std::string_view target) override {
    for (auto& child : children_) {
      if (child != nullptr) {
        child->Seek(target);
      }
    }
    FindSmallest();
    direction_ = kForward;
  }

  void Next() override {
    STRATA_DCHECK(Valid());

    // Ensure that all children are positioned after key().
    // If we are currently moving in the reverse direction, children
    // other than current_ are positioned before key().
    if (direction_ != kForward) {
      std::string current_key(key());
      for (auto& child : children_) {
        if (child != nullptr && child.get() != current_) {
          child->Seek(current_key);
          if (child->Valid() && comparator_->Compare(child->key(), current_key) == 0) {
            child->Next();
          }
        }
      }
      direction_ = kForward;
    }

    current_->Next();
    FindSmallest();
  }

  void Prev() override {
    STRATA_DCHECK(Valid());

    // Ensure that all children are positioned before key().
    // If we are currently moving in the forward direction, children
    // other than current_ are positioned after key().
    if (direction_ != kReverse) {
      std::string current_key(key());
      for (auto& child : children_) {
        if (child != nullptr && child.get() != current_) {
          child->Seek(current_key);
          if (child->Valid()) {
            child->Prev();
          } else {
            child->SeekToLast();
          }
        }
      }
      direction_ = kReverse;
    }

    current_->Prev();
    FindLargest();
  }

 private:
  void FindSmallest() {
    Iterator* smallest = nullptr;
    for (auto& child : children_) {
      if (child != nullptr && child->Valid() &&
          (smallest == nullptr || comparator_->Compare(child->key(), smallest->key()) < 0)) {
        smallest = child.get();
      }
    }
    current_ = smallest;
  }

  void FindLargest() {
    Iterator* largest = nullptr;
    for (auto& child : children_) {
      if (child != nullptr && child->Valid() &&
          (largest == nullptr || comparator_->Compare(child->key(), largest->key()) > 0)) {
        largest = child.get();
      }
    }
    current_ = largest;
  }

  const Comparator* const comparator_;
  std::vector<std::unique_ptr<Iterator>> children_;
  Iterator* current_ = nullptr;
  Direction direction_ = kForward;
};

}  // namespace

std::unique_ptr<Iterator> NewMergingIterator(const Comparator* comparator,
                                             std::vector<std::unique_ptr<Iterator>> children) {
  if (children.empty()) {
    return NewEmptyIterator();
  }
  if (children.size() == 1 && children[0] != nullptr) {
    return std::move(children[0]);
  }
  return std::make_unique<MergingIterator>(comparator, std::move(children));
}

std::unique_ptr<Iterator> NewMergingIterator(const Comparator* comparator,
                                             std::unique_ptr<Iterator>* children, int n) {
  if (n <= 0) {
    return NewEmptyIterator();
  }
  if (n == 1 && children[0] != nullptr) {
    return std::move(children[0]);
  }
  std::vector<std::unique_ptr<Iterator>> vec;
  vec.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    vec.push_back(std::move(children[i]));
  }
  return std::make_unique<MergingIterator>(comparator, std::move(vec));
}

}  // namespace strata

#include "strata/iterator.h"

#include <memory>
#include <string_view>
#include <utility>

#include "strata/status.h"
#include "util/check.h"

namespace strata {
namespace {

// An iterator over nothing. It is never Valid(), so Next/Prev/key/value are
// caller errors (they REQUIRE Valid()) and are caught by STRATA_DCHECK.
class EmptyIterator : public Iterator {
 public:
  explicit EmptyIterator(Status status) : status_(std::move(status)) {}

  bool Valid() const override { return false; }
  void SeekToFirst() override {}
  void SeekToLast() override {}
  void Seek(std::string_view /*target*/) override {}
  void Next() override { STRATA_DCHECK(false); }
  void Prev() override { STRATA_DCHECK(false); }
  std::string_view key() const override {
    STRATA_DCHECK(false);
    return {};
  }
  std::string_view value() const override {
    STRATA_DCHECK(false);
    return {};
  }
  Status status() const override { return status_; }

 private:
  Status status_;
};

}  // namespace

std::unique_ptr<Iterator> NewEmptyIterator() {
  return std::make_unique<EmptyIterator>(Status::OK());
}

std::unique_ptr<Iterator> NewErrorIterator(const Status& status) {
  return std::make_unique<EmptyIterator>(status);
}

}  // namespace strata

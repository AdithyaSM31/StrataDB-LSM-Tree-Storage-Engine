#include "table/two_level_iterator.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "strata/status.h"
#include "util/check.h"

namespace strata {
namespace {

class TwoLevelIterator : public Iterator {
 public:
  TwoLevelIterator(std::unique_ptr<Iterator> index_iter, BlockFunction block_function)
      : block_function_(std::move(block_function)), index_iter_(std::move(index_iter)) {
    STRATA_DCHECK(index_iter_ != nullptr);
    STRATA_DCHECK(block_function_ != nullptr);
  }

  bool Valid() const override { return data_iter_ != nullptr && data_iter_->Valid(); }

  std::string_view key() const override {
    STRATA_DCHECK(Valid());
    return data_iter_->key();
  }

  std::string_view value() const override {
    STRATA_DCHECK(Valid());
    return data_iter_->value();
  }

  Status status() const override {
    // Report the index's error first, then the current block's, then any error
    // remembered from a block we have already moved past.
    if (!index_iter_->status().ok()) {
      return index_iter_->status();
    }
    if (data_iter_ != nullptr && !data_iter_->status().ok()) {
      return data_iter_->status();
    }
    return status_;
  }

  void Seek(std::string_view target) override {
    index_iter_->Seek(target);
    InitDataBlock();
    if (data_iter_ != nullptr) {
      data_iter_->Seek(target);
    }
    SkipEmptyDataBlocksForward();
  }

  void SeekToFirst() override {
    index_iter_->SeekToFirst();
    InitDataBlock();
    if (data_iter_ != nullptr) {
      data_iter_->SeekToFirst();
    }
    SkipEmptyDataBlocksForward();
  }

  void SeekToLast() override {
    index_iter_->SeekToLast();
    InitDataBlock();
    if (data_iter_ != nullptr) {
      data_iter_->SeekToLast();
    }
    SkipEmptyDataBlocksBackward();
  }

  void Next() override {
    STRATA_DCHECK(Valid());
    data_iter_->Next();
    SkipEmptyDataBlocksForward();
  }

  void Prev() override {
    STRATA_DCHECK(Valid());
    data_iter_->Prev();
    SkipEmptyDataBlocksBackward();
  }

 private:
  // Moves to the first entry of the next non-empty block if the current block
  // is used up (or there is none).
  void SkipEmptyDataBlocksForward() {
    while (data_iter_ == nullptr || !data_iter_->Valid()) {
      if (!index_iter_->Valid()) {
        SetDataIterator(nullptr);
        return;
      }
      index_iter_->Next();
      InitDataBlock();
      if (data_iter_ != nullptr) {
        data_iter_->SeekToFirst();
      }
    }
  }

  void SkipEmptyDataBlocksBackward() {
    while (data_iter_ == nullptr || !data_iter_->Valid()) {
      if (!index_iter_->Valid()) {
        SetDataIterator(nullptr);
        return;
      }
      index_iter_->Prev();
      InitDataBlock();
      if (data_iter_ != nullptr) {
        data_iter_->SeekToLast();
      }
    }
  }

  // Replaces the current block's iterator, remembering any error it hit so
  // status() still reports it after we move on.
  void SetDataIterator(std::unique_ptr<Iterator> data_iter) {
    if (data_iter_ != nullptr && status_.ok()) {
      status_ = data_iter_->status();
    }
    data_iter_ = std::move(data_iter);
  }

  // Opens the block the index currently points at, reusing the current block
  // if it is the same one (common when Seek stays within a block).
  void InitDataBlock() {
    if (!index_iter_->Valid()) {
      SetDataIterator(nullptr);
      return;
    }
    const std::string_view handle = index_iter_->value();
    if (data_iter_ != nullptr && handle == data_block_handle_) {
      return;
    }
    data_block_handle_.assign(handle.data(), handle.size());
    SetDataIterator(block_function_(handle));
  }

  BlockFunction block_function_;
  std::unique_ptr<Iterator> index_iter_;
  std::unique_ptr<Iterator> data_iter_;  // null if there is no current block
  std::string data_block_handle_;        // index value of the block in data_iter_
  Status status_;                        // first error from a block we left
};

}  // namespace

std::unique_ptr<Iterator> NewTwoLevelIterator(std::unique_ptr<Iterator> index_iter,
                                              BlockFunction block_function) {
  return std::make_unique<TwoLevelIterator>(std::move(index_iter), std::move(block_function));
}

}  // namespace strata

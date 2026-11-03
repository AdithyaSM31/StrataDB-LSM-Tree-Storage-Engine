#include "db/db_iter.h"

#include <algorithm>
#include <utility>

#include "util/check.h"

namespace strata {

DBIter::DBIter(const Comparator* cmp, std::unique_ptr<Iterator> iter, SequenceNumber sequence)
    : user_comparator_(cmp == nullptr ? BytewiseComparator() : cmp),
      iter_(std::move(iter)),
      sequence_(sequence) {
  STRATA_DCHECK(iter_ != nullptr);
}

DBIter::~DBIter() {
  iter_.reset();
  for (auto it = cleanups_.rbegin(); it != cleanups_.rend(); ++it) {
    (*it)();
  }
}

void DBIter::RegisterCleanup(std::function<void()> cleanup) {
  if (cleanup) {
    cleanups_.push_back(std::move(cleanup));
  }
}

bool DBIter::Valid() const { return valid_; }

std::string_view DBIter::key() const {
  STRATA_DCHECK(valid_);
  return (direction_ == kForward) ? ExtractUserKey(iter_->key()) : saved_key_;
}

std::string_view DBIter::value() const {
  STRATA_DCHECK(valid_);
  return (direction_ == kForward) ? iter_->value() : saved_value_;
}

Status DBIter::status() const {
  if (!status_.ok()) {
    return status_;
  }
  return iter_->status();
}

bool DBIter::ParseKey(ParsedInternalKey* ikey) {
  std::string_view k = iter_->key();
  if (!ParseInternalKey(k, ikey)) {
    status_ = Status::Corruption("corrupted internal key in DBIter");
    return false;
  }
  return true;
}

void DBIter::SeekToFirst() {
  direction_ = kForward;
  ClearSavedValue();
  saved_key_.clear();
  iter_->SeekToFirst();
  if (iter_->Valid()) {
    FindNextUserEntry(false, &saved_key_);
  } else {
    valid_ = false;
  }
}

void DBIter::SeekToLast() {
  direction_ = kReverse;
  ClearSavedValue();
  saved_key_.clear();
  iter_->SeekToLast();
  FindPrevUserEntry();
}

void DBIter::Seek(std::string_view target) {
  direction_ = kForward;
  ClearSavedValue();
  saved_key_.clear();
  AppendInternalKey(&saved_key_, ParsedInternalKey{target, sequence_, kValueTypeForSeek});
  iter_->Seek(saved_key_);
  if (iter_->Valid()) {
    FindNextUserEntry(false, &saved_key_);
  } else {
    valid_ = false;
  }
}

void DBIter::FindNextUserEntry(bool skipping, std::string* skip) {
  STRATA_DCHECK(iter_->Valid());
  STRATA_DCHECK(direction_ == kForward);
  do {
    ParsedInternalKey ikey;
    if (!ParseKey(&ikey)) {
      valid_ = false;
      return;
    }
    if (ikey.sequence <= sequence_) {
      switch (ikey.type) {
        case kTypeDeletion:
          skip->assign(ikey.user_key.data(), ikey.user_key.size());
          skipping = true;
          break;
        case kTypeValue:
          if (skipping && user_comparator_->Compare(ikey.user_key, *skip) <= 0) {
            // Entry hidden
          } else {
            valid_ = true;
            saved_key_.clear();
            return;
          }
          break;
      }
    }
    iter_->Next();
  } while (iter_->Valid());
  saved_key_.clear();
  valid_ = false;
}

void DBIter::Next() {
  STRATA_DCHECK(valid_);

  if (direction_ == kReverse) {
    direction_ = kForward;
    // iter_ is pointing before saved_key_. Seek to saved_key_.
    iter_->Seek(MakeInternalKey(saved_key_, sequence_, kValueTypeForSeek));
    if (!iter_->Valid()) {
      valid_ = false;
      saved_key_.clear();
      ClearSavedValue();
      return;
    }
  } else {
    saved_key_.assign(ExtractUserKey(iter_->key()));
  }

  FindNextUserEntry(true, &saved_key_);
}

void DBIter::Prev() {
  STRATA_DCHECK(valid_);

  if (direction_ == kForward) {
    saved_key_.assign(ExtractUserKey(iter_->key()));
    while (true) {
      iter_->Prev();
      if (!iter_->Valid()) {
        valid_ = false;
        direction_ = kReverse;
        ClearSavedValue();
        return;
      }
      ParsedInternalKey ikey;
      if (!ParseKey(&ikey)) {
        valid_ = false;
        direction_ = kReverse;
        ClearSavedValue();
        return;
      }
      if (user_comparator_->Compare(ikey.user_key, saved_key_) < 0) {
        break;
      }
    }
    direction_ = kReverse;
  }

  FindPrevUserEntry();
}

void DBIter::FindPrevUserEntry() {
  STRATA_DCHECK(direction_ == kReverse);

  ValueType value_type = kTypeDeletion;
  if (iter_->Valid()) {
    do {
      ParsedInternalKey ikey;
      if (!ParseKey(&ikey)) {
        valid_ = false;
        ClearSavedValue();
        return;
      }
      if (ikey.sequence <= sequence_) {
        if (value_type != kTypeDeletion &&
            user_comparator_->Compare(ikey.user_key, saved_key_) < 0) {
          // We encountered a key smaller than saved_key_, and we already
          // have an acceptable entry for saved_key_. We are done.
          break;
        }
        value_type = ikey.type;
        if (value_type == kTypeDeletion) {
          saved_key_.clear();
          ClearSavedValue();
        } else {
          saved_key_.assign(ikey.user_key.data(), ikey.user_key.size());
          saved_value_.assign(iter_->value().data(), iter_->value().size());
        }
      }
      iter_->Prev();
    } while (iter_->Valid());
  }

  if (value_type == kTypeDeletion) {
    valid_ = false;
    saved_key_.clear();
    ClearSavedValue();
    direction_ = kForward;
  } else {
    valid_ = true;
  }
}

std::unique_ptr<DBIter> NewDBIterator(const Comparator* cmp, std::unique_ptr<Iterator> iter,
                                      SequenceNumber sequence) {
  return std::make_unique<DBIter>(cmp, std::move(iter), sequence);
}

}  // namespace strata

#include "memtable/memtable.h"

#include <cstring>
#include <memory>

#include "util/check.h"
#include "util/coding.h"

namespace strata {

static inline std::string_view GetLengthPrefixedSlice(const char* p) {
  uint32_t len;
  const char* data = GetVarint32Ptr(p, p + 5, &len);
  STRATA_DCHECK(data != nullptr);
  return std::string_view(data, len);
}

int MemTable::KeyComparator::operator()(const char* a, const char* b) const {
  std::string_view a_key = GetLengthPrefixedSlice(a);
  std::string_view b_key = GetLengthPrefixedSlice(b);
  return comparator.Compare(a_key, b_key);
}

MemTable::MemTable(const InternalKeyComparator& cmp)
    : comparator_(cmp), refs_(0), arena_(), table_(comparator_, &arena_) {}

MemTable::~MemTable() { STRATA_DCHECK(refs_ == 0); }

void MemTable::Ref() { ++refs_; }

void MemTable::Unref() {
  STRATA_DCHECK(refs_ > 0);
  --refs_;
  if (refs_ <= 0) {
    delete this;
  }
}

size_t MemTable::ApproximateMemoryUsage() const { return arena_.MemoryUsage(); }

bool MemTable::IsEmpty() const {
  SkipList<const char*, KeyComparator>::Iterator iter(&table_);
  iter.SeekToFirst();
  return !iter.Valid();
}

void MemTable::Add(SequenceNumber seq, ValueType type, std::string_view key,
                   std::string_view value) {
  STRATA_DCHECK(seq <= kMaxSequenceNumber);
  STRATA_DCHECK(static_cast<uint8_t>(type) <= static_cast<uint8_t>(kTypeValue));

  const size_t key_size = key.size();
  const size_t val_size = value.size();
  const size_t internal_key_size = key_size + 8;
  STRATA_CHECK(internal_key_size <= UINT32_MAX);
  STRATA_CHECK(val_size <= UINT32_MAX);
  const size_t encoded_len =
      VarintLength(internal_key_size) + internal_key_size + VarintLength(val_size) + val_size;

  char* const buf = arena_.Allocate(encoded_len);
  char* p = EncodeVarint32(buf, static_cast<uint32_t>(internal_key_size));
  if (key_size > 0) {
    std::memcpy(p, key.data(), key_size);
  }
  p += key_size;
  EncodeFixed64(p, PackSequenceAndType(seq, type));
  p += 8;
  p = EncodeVarint32(p, static_cast<uint32_t>(val_size));
  if (val_size > 0) {
    std::memcpy(p, value.data(), val_size);
  }
  p += val_size;
  STRATA_DCHECK(p == buf + encoded_len);

  table_.Insert(buf);
}

bool MemTable::Get(const LookupKey& key, std::string* value, Status* s) {
  SkipList<const char*, KeyComparator>::Iterator iter(&table_);
  iter.Seek(key.memtable_key().data());
  if (!iter.Valid()) {
    return false;
  }

  const char* entry = iter.key();
  uint32_t key_length = 0;
  const char* key_ptr = GetVarint32Ptr(entry, entry + 5, &key_length);
  STRATA_DCHECK(key_ptr != nullptr);
  STRATA_DCHECK(key_length >= 8);
  if (key_ptr == nullptr || key_length < 8) {
    return false;
  }

  std::string_view entry_user_key(key_ptr, key_length - 8);
  if (entry_user_key != key.user_key()) {
    return false;
  }

  const uint64_t trailer = DecodeFixed64(key_ptr + key_length - 8);
  const auto type = static_cast<ValueType>(trailer & 0xff);
  if (type == kTypeValue) {
    std::string_view val = GetLengthPrefixedSlice(key_ptr + key_length);
    if (value != nullptr) {
      value->assign(val.data(), val.size());
    }
    if (s != nullptr) {
      *s = Status::OK();
    }
    return true;
  }
  if (type == kTypeDeletion) {
    if (s != nullptr) {
      *s = Status::NotFound("Key deleted");
    }
    return true;
  }
  return false;
}

namespace {

class MemTableIterator : public Iterator {
 public:
  explicit MemTableIterator(SkipList<const char*, MemTable::KeyComparator>* table) : iter_(table) {}
  ~MemTableIterator() override = default;

  bool Valid() const override { return iter_.Valid(); }

  void SeekToFirst() override { iter_.SeekToFirst(); }

  void SeekToLast() override { iter_.SeekToLast(); }

  void Seek(std::string_view target) override {
    tmp_.clear();
    PutLengthPrefixed(&tmp_, target);
    iter_.Seek(tmp_.data());
  }

  void Next() override {
    STRATA_DCHECK(Valid());
    if (Valid()) {
      iter_.Next();
    }
  }

  void Prev() override {
    STRATA_DCHECK(Valid());
    if (Valid()) {
      iter_.Prev();
    }
  }

  std::string_view key() const override {
    STRATA_DCHECK(Valid());
    return Valid() ? GetLengthPrefixedSlice(iter_.key()) : std::string_view();
  }

  std::string_view value() const override {
    STRATA_DCHECK(Valid());
    if (!Valid()) {
      return std::string_view();
    }
    std::string_view key_slice = GetLengthPrefixedSlice(iter_.key());
    return GetLengthPrefixedSlice(key_slice.data() + key_slice.size());
  }

  Status status() const override { return Status::OK(); }

 private:
  SkipList<const char*, MemTable::KeyComparator>::Iterator iter_;
  std::string tmp_;
};

}  // namespace

std::unique_ptr<Iterator> MemTable::NewIterator() {
  return std::make_unique<MemTableIterator>(&table_);
}

}  // namespace strata

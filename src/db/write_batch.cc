#include "strata/write_batch.h"

#include <cstdint>
#include <string_view>

#include "db/dbformat.h"
#include "db/write_batch_internal.h"
#include "memtable/memtable.h"
#include "util/check.h"
#include "util/coding.h"

namespace strata {

static constexpr size_t kHeaderSize = 12;

WriteBatch::WriteBatch() { Clear(); }

void WriteBatch::Clear() { rep_.assign(kHeaderSize, '\0'); }

size_t WriteBatch::ApproximateSize() const { return rep_.size(); }

void WriteBatch::Put(std::string_view key, std::string_view value) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  rep_.push_back(static_cast<char>(kTypeValue));
  PutLengthPrefixed(&rep_, key);
  PutLengthPrefixed(&rep_, value);
}

void WriteBatch::Delete(std::string_view key) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  rep_.push_back(static_cast<char>(kTypeDeletion));
  PutLengthPrefixed(&rep_, key);
}

void WriteBatch::Append(const WriteBatch& source) {
  const uint32_t src_count = WriteBatchInternal::Count(&source);
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + src_count);
  if (source.rep_.size() > kHeaderSize) {
    rep_.append(source.rep_.data() + kHeaderSize, source.rep_.size() - kHeaderSize);
  }
}

Status WriteBatch::Iterate(Handler* handler) const {
  if (rep_.size() < kHeaderSize) {
    return Status::Corruption("malformed WriteBatch (too small)");
  }

  const uint32_t count = DecodeFixed32(rep_.data() + 8);
  std::string_view input(rep_.data() + kHeaderSize, rep_.size() - kHeaderSize);
  uint32_t found = 0;

  // Validation pass: ensure the batch is completely well-formed before modifying state.
  while (!input.empty()) {
    ++found;
    const auto tag = static_cast<uint8_t>(input[0]);
    input.remove_prefix(1);
    if (tag == static_cast<uint8_t>(kTypeValue)) {
      std::string_view key;
      std::string_view value;
      if (!GetLengthPrefixed(&input, &key) || !GetLengthPrefixed(&input, &value)) {
        return Status::Corruption("bad WriteBatch Put");
      }
    } else if (tag == static_cast<uint8_t>(kTypeDeletion)) {
      std::string_view key;
      if (!GetLengthPrefixed(&input, &key)) {
        return Status::Corruption("bad WriteBatch Delete");
      }
    } else {
      return Status::Corruption("unknown WriteBatch tag");
    }
  }

  if (found != count) {
    return Status::Corruption("WriteBatch has wrong count");
  }

  // Execution pass: dispatch to handler.
  input = std::string_view(rep_.data() + kHeaderSize, rep_.size() - kHeaderSize);
  while (!input.empty()) {
    const auto tag = static_cast<uint8_t>(input[0]);
    input.remove_prefix(1);
    if (tag == static_cast<uint8_t>(kTypeValue)) {
      std::string_view key;
      std::string_view value;
      GetLengthPrefixed(&input, &key);
      GetLengthPrefixed(&input, &value);
      handler->Put(key, value);
    } else {
      std::string_view key;
      GetLengthPrefixed(&input, &key);
      handler->Delete(key);
    }
  }

  return Status::OK();
}

uint32_t WriteBatchInternal::Count(const WriteBatch* b) {
  if (b->rep_.size() < kHeaderSize) {
    return 0;
  }
  return DecodeFixed32(b->rep_.data() + 8);
}

void WriteBatchInternal::SetCount(WriteBatch* b, uint32_t count) {
  if (b->rep_.size() < kHeaderSize) {
    b->rep_.resize(kHeaderSize, '\0');
  }
  EncodeFixed32(b->rep_.data() + 8, count);
}

SequenceNumber WriteBatchInternal::Sequence(const WriteBatch* b) {
  if (b->rep_.size() < 8) {
    return 0;
  }
  return DecodeFixed64(b->rep_.data());
}

void WriteBatchInternal::SetSequence(WriteBatch* b, SequenceNumber seq) {
  if (b->rep_.size() < 8) {
    b->rep_.resize(kHeaderSize, '\0');
  }
  EncodeFixed64(b->rep_.data(), seq);
}

std::string_view WriteBatchInternal::Contents(const WriteBatch* b) { return b->rep_; }

void WriteBatchInternal::SetContents(WriteBatch* b, std::string_view contents) {
  b->rep_.assign(contents.data(), contents.size());
}

void WriteBatchInternal::Append(WriteBatch* dst, const WriteBatch* src) { dst->Append(*src); }

namespace {

class MemTableInserter : public WriteBatch::Handler {
 public:
  MemTableInserter(SequenceNumber seq, MemTable* mem) : seq_(seq), mem_(mem) {}
  ~MemTableInserter() override = default;

  void Put(std::string_view key, std::string_view value) override {
    mem_->Add(seq_++, kTypeValue, key, value);
  }

  void Delete(std::string_view key) override { mem_->Add(seq_++, kTypeDeletion, key, {}); }

 private:
  SequenceNumber seq_;
  MemTable* mem_;
};

}  // namespace

Status WriteBatchInternal::InsertInto(const WriteBatch* b, MemTable* mem) {
  MemTableInserter inserter(Sequence(b), mem);
  return b->Iterate(&inserter);
}

}  // namespace strata

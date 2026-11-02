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
  if (source.rep_.size() > kHeaderSize) {
    const std::string_view records(source.rep_.data() + kHeaderSize,
                                   source.rep_.size() - kHeaderSize);
    if (&source == this) {
      const std::string copy(records);
      WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + src_count);
      rep_.append(copy);
      return;
    }
    WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + src_count);
    rep_.append(records.data(), records.size());
  } else {
    WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + src_count);
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

  if (handler == nullptr) {
    return Status::OK();
  }

  // Execution pass: dispatch to handler.
  input = std::string_view(rep_.data() + kHeaderSize, rep_.size() - kHeaderSize);
  while (!input.empty()) {
    const auto tag = static_cast<uint8_t>(input[0]);
    input.remove_prefix(1);
    if (tag == static_cast<uint8_t>(kTypeValue)) {
      std::string_view key;
      std::string_view value;
      // The first pass already validated every record; the DCHECK keeps the
      // two passes from drifting apart.
      const bool ok = GetLengthPrefixed(&input, &key) && GetLengthPrefixed(&input, &value);
      STRATA_DCHECK(ok);
      handler->Put(key, value);
    } else {
      std::string_view key;
      const bool ok = GetLengthPrefixed(&input, &key);
      STRATA_DCHECK(ok);
      handler->Delete(key);
    }
  }

  return Status::OK();
}

uint32_t WriteBatchInternal::Count(const WriteBatch* b) {
  STRATA_DCHECK(b != nullptr);
  if (b->rep_.size() < kHeaderSize) {
    return 0;
  }
  return DecodeFixed32(b->rep_.data() + 8);
}

void WriteBatchInternal::SetCount(WriteBatch* b, uint32_t count) {
  STRATA_DCHECK(b != nullptr);
  if (b->rep_.size() < kHeaderSize) {
    b->rep_.resize(kHeaderSize, '\0');
  }
  EncodeFixed32(b->rep_.data() + 8, count);
}

SequenceNumber WriteBatchInternal::Sequence(const WriteBatch* b) {
  STRATA_DCHECK(b != nullptr);
  if (b->rep_.size() < 8) {
    return 0;
  }
  return DecodeFixed64(b->rep_.data());
}

void WriteBatchInternal::SetSequence(WriteBatch* b, SequenceNumber seq) {
  STRATA_DCHECK(b != nullptr);
  if (b->rep_.size() < kHeaderSize) {
    b->rep_.resize(kHeaderSize, '\0');
  }
  EncodeFixed64(b->rep_.data(), seq);
}

std::string_view WriteBatchInternal::Contents(const WriteBatch* b) {
  STRATA_DCHECK(b != nullptr);
  return b->rep_;
}

void WriteBatchInternal::SetContents(WriteBatch* b, std::string_view contents) {
  STRATA_DCHECK(b != nullptr);
  b->rep_.assign(contents.data(), contents.size());
}

void WriteBatchInternal::Append(WriteBatch* dst, const WriteBatch* src) {
  STRATA_DCHECK(dst != nullptr);
  STRATA_DCHECK(src != nullptr);
  dst->Append(*src);
}

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
  STRATA_DCHECK(b != nullptr);
  STRATA_DCHECK(mem != nullptr);
  MemTableInserter inserter(Sequence(b), mem);
  return b->Iterate(&inserter);
}

}  // namespace strata

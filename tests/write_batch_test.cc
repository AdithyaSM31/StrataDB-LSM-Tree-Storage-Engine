#include "strata/write_batch.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "db/dbformat.h"
#include "db/write_batch_internal.h"
#include "memtable/memtable.h"
#include "strata/status.h"
#include "util/coding.h"

namespace strata {
namespace {

class RecordingHandler : public WriteBatch::Handler {
 public:
  struct Operation {
    enum Type { kPut, kDelete };
    Type type;
    std::string key;
    std::string value;
  };

  void Put(std::string_view key, std::string_view value) override {
    ops.push_back({Operation::kPut, std::string(key), std::string(value)});
  }

  void Delete(std::string_view key) override {
    ops.push_back({Operation::kDelete, std::string(key), {}});
  }

  std::vector<Operation> ops;
};

TEST(WriteBatchTest, EmptyBatch) {
  WriteBatch batch;
  EXPECT_EQ(WriteBatchInternal::Count(&batch), 0u);
  EXPECT_EQ(batch.ApproximateSize(), 12u);

  RecordingHandler handler;
  Status s = batch.Iterate(&handler);
  EXPECT_TRUE(s.ok());
  EXPECT_TRUE(handler.ops.empty());

  // Clear resets back to 12 zero bytes.
  batch.Clear();
  EXPECT_EQ(WriteBatchInternal::Count(&batch), 0u);
  EXPECT_EQ(batch.ApproximateSize(), 12u);
}

TEST(WriteBatchTest, OperationsAndMemTable) {
  WriteBatch batch;
  batch.Put("key1", "val1");
  batch.Delete("key2");
  batch.Put("key1", "val2");  // Same key: later Put should win!

  EXPECT_EQ(WriteBatchInternal::Count(&batch), 3u);

  WriteBatchInternal::SetSequence(&batch, 100);
  EXPECT_EQ(WriteBatchInternal::Sequence(&batch), 100u);

  InternalKeyComparator cmp;
  MemTable* mem = new MemTable(cmp);
  mem->Ref();

  Status s = WriteBatchInternal::InsertInto(&batch, mem);
  EXPECT_TRUE(s.ok());

  // Check that the later Put to "key1" wins (at seq 102).
  {
    std::string value;
    Status get_status;
    bool found = mem->Get(LookupKey("key1", 102), &value, &get_status);
    EXPECT_TRUE(found);
    EXPECT_TRUE(get_status.ok());
    EXPECT_EQ(value, "val2");
  }

  // Check that an earlier snapshot at seq 100 sees "val1".
  {
    std::string value;
    Status get_status;
    bool found = mem->Get(LookupKey("key1", 100), &value, &get_status);
    EXPECT_TRUE(found);
    EXPECT_TRUE(get_status.ok());
    EXPECT_EQ(value, "val1");
  }

  // Check that key2 is seen as deleted at seq 101.
  {
    std::string value;
    Status get_status;
    bool found = mem->Get(LookupKey("key2", 101), &value, &get_status);
    EXPECT_TRUE(found);
    EXPECT_TRUE(get_status.IsNotFound());
  }

  // Verify all 3 entries are present in memtable with sequences 100, 101, 102.
  std::unique_ptr<Iterator> it = mem->NewIterator();
  std::vector<std::pair<SequenceNumber, ValueType>> entries;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    std::string_view ikey = it->key();
    ASSERT_GE(ikey.size(), 8u);
    uint64_t trailer = DecodeFixed64(ikey.data() + ikey.size() - 8);
    SequenceNumber seq = trailer >> 8;
    auto type = static_cast<ValueType>(trailer & 0xff);
    entries.emplace_back(seq, type);
  }

  // Internal keys are sorted by user_key ascending, then sequence descending:
  // "key1" seq 102 (kTypeValue)
  // "key1" seq 100 (kTypeValue)
  // "key2" seq 101 (kTypeDeletion)
  ASSERT_EQ(entries.size(), 3u);
  EXPECT_EQ(entries[0].first, 102u);
  EXPECT_EQ(entries[0].second, kTypeValue);
  EXPECT_EQ(entries[1].first, 100u);
  EXPECT_EQ(entries[1].second, kTypeValue);
  EXPECT_EQ(entries[2].first, 101u);
  EXPECT_EQ(entries[2].second, kTypeDeletion);

  mem->Unref();
}

TEST(WriteBatchTest, Append) {
  WriteBatch b1;
  WriteBatchInternal::SetSequence(&b1, 100);
  b1.Put("k1", "v1");
  b1.Put("k2", "v2");

  WriteBatch b2;
  WriteBatchInternal::SetSequence(&b2, 200);
  b2.Put("k3", "v3");
  b2.Delete("k4");

  b1.Append(b2);

  // Keeps destination sequence, adds counts
  EXPECT_EQ(WriteBatchInternal::Sequence(&b1), 100u);
  EXPECT_EQ(WriteBatchInternal::Count(&b1), 4u);

  RecordingHandler handler;
  Status s = b1.Iterate(&handler);
  EXPECT_TRUE(s.ok());
  ASSERT_EQ(handler.ops.size(), 4u);
  EXPECT_EQ(handler.ops[0].key, "k1");
  EXPECT_EQ(handler.ops[0].value, "v1");
  EXPECT_EQ(handler.ops[1].key, "k2");
  EXPECT_EQ(handler.ops[1].value, "v2");
  EXPECT_EQ(handler.ops[2].key, "k3");
  EXPECT_EQ(handler.ops[2].value, "v3");
  EXPECT_EQ(handler.ops[3].key, "k4");
  EXPECT_EQ(handler.ops[3].type, RecordingHandler::Operation::kDelete);

  // Appending an empty batch should be a no-op
  WriteBatch empty;
  b1.Append(empty);
  EXPECT_EQ(WriteBatchInternal::Sequence(&b1), 100u);
  EXPECT_EQ(WriteBatchInternal::Count(&b1), 4u);

  // Appending to an empty batch
  WriteBatch dst;
  WriteBatchInternal::SetSequence(&dst, 50);
  dst.Append(b1);
  EXPECT_EQ(WriteBatchInternal::Sequence(&dst), 50u);
  EXPECT_EQ(WriteBatchInternal::Count(&dst), 4u);
}

TEST(WriteBatchTest, SetContentsRoundTrip) {
  WriteBatch b1;
  WriteBatchInternal::SetSequence(&b1, 999);
  b1.Put("hello", "world");
  b1.Delete("goodbye");
  b1.Put("foo", "bar");

  std::string_view contents = WriteBatchInternal::Contents(&b1);
  EXPECT_EQ(contents.size(), b1.ApproximateSize());

  WriteBatch b2;
  WriteBatchInternal::SetContents(&b2, contents);

  EXPECT_EQ(WriteBatchInternal::Sequence(&b2), 999u);
  EXPECT_EQ(WriteBatchInternal::Count(&b2), 3u);
  EXPECT_EQ(WriteBatchInternal::Contents(&b2), contents);

  RecordingHandler h1;
  RecordingHandler h2;
  EXPECT_TRUE(b1.Iterate(&h1).ok());
  EXPECT_TRUE(b2.Iterate(&h2).ok());
  ASSERT_EQ(h1.ops.size(), h2.ops.size());
  for (size_t i = 0; i < h1.ops.size(); ++i) {
    EXPECT_EQ(h1.ops[i].type, h2.ops[i].type);
    EXPECT_EQ(h1.ops[i].key, h2.ops[i].key);
    EXPECT_EQ(h1.ops[i].value, h2.ops[i].value);
  }
}

TEST(WriteBatchTest, Corruption) {
  WriteBatch batch;
  WriteBatchInternal::SetSequence(&batch, 50);
  batch.Put("alpha", "111");
  batch.Delete("beta");
  batch.Put("gamma", "333");
  ASSERT_EQ(WriteBatchInternal::Count(&batch), 3u);

  const std::string valid = std::string(WriteBatchInternal::Contents(&batch));

  // 1. Cut the batch at every byte position from 0 to valid.size() - 1:
  for (size_t cut_len = 0; cut_len < valid.size(); ++cut_len) {
    WriteBatch b;
    WriteBatchInternal::SetContents(&b, std::string_view(valid.data(), cut_len));

    RecordingHandler handler;
    Status s = b.Iterate(&handler);
    EXPECT_TRUE(s.IsCorruption()) << "Expected corruption at cut_len " << cut_len;
    EXPECT_TRUE(handler.ops.empty())
        << "Validate-first failed: handler called at cut_len " << cut_len;

    InternalKeyComparator cmp;
    MemTable* mem = new MemTable(cmp);
    mem->Ref();
    s = WriteBatchInternal::InsertInto(&b, mem);
    EXPECT_TRUE(s.IsCorruption()) << "Expected corruption at cut_len " << cut_len;

    std::unique_ptr<Iterator> it = mem->NewIterator();
    it->SeekToFirst();
    EXPECT_FALSE(it->Valid()) << "MemTable must have 0 entries after failed InsertInto at cut_len "
                              << cut_len;
    mem->Unref();
  }

  // 2. Unknown tag:
  {
    std::string corrupted = valid;
    // Tag for first record is at offset 12
    corrupted[12] = 2;  // Not kTypeValue (1) and not kTypeDeletion (0)
    WriteBatch b;
    WriteBatchInternal::SetContents(&b, corrupted);

    RecordingHandler handler;
    Status s = b.Iterate(&handler);
    EXPECT_TRUE(s.IsCorruption());
    EXPECT_EQ(s.message(), "unknown WriteBatch tag");
    EXPECT_TRUE(handler.ops.empty());

    InternalKeyComparator cmp;
    MemTable* mem = new MemTable(cmp);
    mem->Ref();
    s = WriteBatchInternal::InsertInto(&b, mem);
    EXPECT_TRUE(s.IsCorruption());
    std::unique_ptr<Iterator> it = mem->NewIterator();
    it->SeekToFirst();
    EXPECT_FALSE(it->Valid());
    mem->Unref();
  }

  // 3. Wrong count:
  {
    // Count too high
    std::string corrupted = valid;
    EncodeFixed32(&corrupted[8], 4);  // Actual is 3
    WriteBatch b;
    WriteBatchInternal::SetContents(&b, corrupted);

    RecordingHandler handler;
    Status s = b.Iterate(&handler);
    EXPECT_TRUE(s.IsCorruption());
    EXPECT_EQ(s.message(), "WriteBatch has wrong count");
    EXPECT_TRUE(handler.ops.empty());

    InternalKeyComparator cmp;
    MemTable* mem = new MemTable(cmp);
    mem->Ref();
    s = WriteBatchInternal::InsertInto(&b, mem);
    EXPECT_TRUE(s.IsCorruption());
    std::unique_ptr<Iterator> it = mem->NewIterator();
    it->SeekToFirst();
    EXPECT_FALSE(it->Valid());
    mem->Unref();
  }

  {
    // Count too low
    std::string corrupted = valid;
    EncodeFixed32(&corrupted[8], 2);  // Actual is 3
    WriteBatch b;
    WriteBatchInternal::SetContents(&b, corrupted);

    RecordingHandler handler;
    Status s = b.Iterate(&handler);
    EXPECT_TRUE(s.IsCorruption());
    EXPECT_EQ(s.message(), "WriteBatch has wrong count");
    EXPECT_TRUE(handler.ops.empty());

    InternalKeyComparator cmp;
    MemTable* mem = new MemTable(cmp);
    mem->Ref();
    s = WriteBatchInternal::InsertInto(&b, mem);
    EXPECT_TRUE(s.IsCorruption());
    std::unique_ptr<Iterator> it = mem->NewIterator();
    it->SeekToFirst();
    EXPECT_FALSE(it->Valid());
    mem->Unref();
  }

  // 4. Bad Put / Delete length prefixes:
  {
    // Bad Put length prefix running past end
    std::string corrupted = valid;
    // Offset 12 is tag, 13 is key length varint
    corrupted[13] = static_cast<char>(0x7f);  // key length = 127 bytes, runs past end
    WriteBatch b;
    WriteBatchInternal::SetContents(&b, corrupted);

    RecordingHandler handler;
    Status s = b.Iterate(&handler);
    EXPECT_TRUE(s.IsCorruption());
    EXPECT_EQ(s.message(), "bad WriteBatch Put");
    EXPECT_TRUE(handler.ops.empty());
  }

  {
    // Bad Delete length prefix running past end
    // Find offset of second record (Delete "beta")
    // Tag 1 (1 byte), key "alpha" (1 + 5), val "111" (1 + 3) -> 12 + 1 + 6 + 4 = 23
    size_t delete_tag_offset = 12 + 1 + 1 + 5 + 1 + 3;
    ASSERT_EQ(static_cast<uint8_t>(valid[delete_tag_offset]), 0);  // kTypeDeletion
    std::string corrupted = valid;
    corrupted[delete_tag_offset + 1] = static_cast<char>(0x7f);  // key length = 127 bytes
    WriteBatch b;
    WriteBatchInternal::SetContents(&b, corrupted);

    RecordingHandler handler;
    Status s = b.Iterate(&handler);
    EXPECT_TRUE(s.IsCorruption());
    EXPECT_EQ(s.message(), "bad WriteBatch Delete");
    EXPECT_TRUE(handler.ops.empty());
  }
}

}  // namespace
}  // namespace strata

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "db/dbformat.h"
#include "db/filename.h"
#include "strata/db.h"
#include "strata/env.h"
#include "strata/iterator.h"
#include "strata/options.h"
#include "strata/status.h"
#include "table/format.h"
#include "table/table.h"
#include "table/table_builder.h"
#include "util/random.h"

namespace strata {
namespace {

const InternalKeyComparator kIcmp;

class StringSink : public WritableFile {
 public:
  Status Append(std::string_view data) override {
    contents_.append(data.data(), data.size());
    return Status::OK();
  }
  Status Flush() override { return Status::OK(); }
  Status Sync() override { return Status::OK(); }
  Status Close() override { return Status::OK(); }

  const std::string& contents() const { return contents_; }

 private:
  std::string contents_;
};

class StringSource : public RandomAccessFile {
 public:
  explicit StringSource(std::string contents) : contents_(std::move(contents)) {}
  Status Read(uint64_t offset, size_t n, std::string_view* result, char* scratch) const override {
    if (offset >= contents_.size()) {
      *result = {};
      return Status::OK();
    }
    const size_t len = std::min(n, static_cast<size_t>(contents_.size() - offset));
    std::memcpy(scratch, contents_.data() + offset, len);
    *result = std::string_view(scratch, len);
    return Status::OK();
  }

 private:
  std::string contents_;
};

struct Entry {
  std::string key;
  std::string value;
};

std::string MakeInternalKey(std::string_view user_key, SequenceNumber seq) {
  ParsedInternalKey ikey(user_key, seq, kTypeValue);
  std::string result;
  AppendInternalKey(&result, ikey);
  return result;
}

std::vector<Entry> GenerateEntries(int count) {
  std::vector<Entry> entries;
  entries.reserve(count);
  for (int i = 0; i < count; ++i) {
    char ukey[32];
    char val[64];
    std::snprintf(ukey, sizeof(ukey), "key_%06d", i);
    std::snprintf(val, sizeof(val), "val_payload_strata_%06d", i);
    entries.push_back({MakeInternalKey(ukey, 100 + i), std::string(val)});
  }
  std::sort(entries.begin(), entries.end(),
            [](const Entry& a, const Entry& b) { return kIcmp.Compare(a.key, b.key) < 0; });
  return entries;
}

Options SSTOptions(size_t block_size = 256) {
  Options options;
  options.block_size = block_size;
  options.block_restart_interval = 4;
  options.bloom_bits_per_key = 10;
  return options;
}

std::string BuildTableString(const Options& options, const std::vector<Entry>& entries) {
  StringSink sink;
  TableBuilder builder(options, &kIcmp, &sink);
  for (const auto& e : entries) {
    builder.Add(e.key, e.value);
  }
  EXPECT_TRUE(builder.Finish().ok());
  return sink.contents();
}

void FlipBit(std::string* s, size_t pos, int bit) {
  ASSERT_LT(pos, s->size());
  (*s)[pos] ^= static_cast<char>(1 << bit);
}

TEST(SSTableCorruptionTest, FooterMagicNumberBitFlipFailsOpen) {
  const auto entries = GenerateEntries(50);
  const Options options = SSTOptions();
  const std::string original = BuildTableString(options, entries);
  ASSERT_GE(original.size(), Footer::kEncodedLength);

  // Magic number is the last 8 bytes of the footer.
  const size_t magic_offset = original.size() - 8;
  for (size_t byte_idx = magic_offset; byte_idx < original.size(); ++byte_idx) {
    for (int bit = 0; bit < 8; ++bit) {
      std::string corrupted = original;
      FlipBit(&corrupted, byte_idx, bit);

      std::unique_ptr<Table> table;
      auto file = std::make_unique<StringSource>(corrupted);
      Status s = Table::Open(options, &kIcmp, std::move(file), corrupted.size(), &table);

      EXPECT_FALSE(s.ok()) << "Expected failure on magic bit flip at byte " << byte_idx;
      EXPECT_TRUE(s.IsCorruption()) << s.ToString();
    }
  }
}

TEST(SSTableCorruptionTest, FooterHandlesBitFlipFailsOpen) {
  const auto entries = GenerateEntries(50);
  const Options options = SSTOptions();
  const std::string original = BuildTableString(options, entries);
  ASSERT_GE(original.size(), Footer::kEncodedLength);

  // Footer block handles occupy the first 2 * BlockHandle::kMaxEncodedLength bytes of the footer.
  const size_t footer_start = original.size() - Footer::kEncodedLength;
  for (size_t byte_idx = footer_start; byte_idx < footer_start + 20; byte_idx += 2) {
    std::string corrupted = original;
    FlipBit(&corrupted, byte_idx, 0);

    std::unique_ptr<Table> table;
    auto file = std::make_unique<StringSource>(corrupted);
    Status s = Table::Open(options, &kIcmp, std::move(file), corrupted.size(), &table);

    // Either Open fails with corruption or opening succeeds but subsequent block reads fail.
    if (!s.ok()) {
      EXPECT_TRUE(s.IsCorruption()) << s.ToString();
    }
  }
}

TEST(SSTableCorruptionTest, DataBlockBitFlipsNeverReturnWrongData) {
  const auto entries = GenerateEntries(60);
  const Options options = SSTOptions(256);  // small blocks to yield multiple blocks
  const std::string original = BuildTableString(options, entries);

  // Test bit flips across various offsets within data blocks (first 50% of the table file)
  const size_t test_limit = original.size() / 2;
  for (size_t offset = 0; offset < test_limit; offset += 17) {
    for (int bit : {0, 3, 7}) {
      std::string corrupted = original;
      FlipBit(&corrupted, offset, bit);

      std::unique_ptr<Table> table;
      auto file = std::make_unique<StringSource>(corrupted);
      Status open_status = Table::Open(options, &kIcmp, std::move(file), corrupted.size(), &table);
      if (!open_status.ok()) {
        EXPECT_TRUE(open_status.IsCorruption()) << open_status.ToString();
        continue;
      }

      // Check all entries: InternalGet must return either the exact correct value or Corruption.
      // It must NEVER return a corrupted/wrong value!
      for (const auto& entry : entries) {
        bool found = false;
        std::string found_k;
        std::string found_v;
        ReadOptions ropts;
        Status s = table->InternalGet(ropts, entry.key, &found, &found_k, &found_v);
        if (s.ok() && found) {
          EXPECT_EQ(found_v, entry.value)
              << "Corruption silent payload mutation detected at offset " << offset;
          EXPECT_EQ(found_k, entry.key);
        } else if (!s.ok()) {
          EXPECT_TRUE(s.IsCorruption())
              << "Non-corruption error returned on bit flip: " << s.ToString();
        }
      }
    }
  }
}

TEST(SSTableCorruptionTest, DataBlockBitFlipsDetectedByIteratorScan) {
  const auto entries = GenerateEntries(50);
  const Options options = SSTOptions(256);
  const std::string original = BuildTableString(options, entries);

  // Flip a bit in the first data block
  std::string corrupted = original;
  FlipBit(&corrupted, 20, 1);

  std::unique_ptr<Table> table;
  auto file = std::make_unique<StringSource>(corrupted);
  ASSERT_TRUE(Table::Open(options, &kIcmp, std::move(file), corrupted.size(), &table).ok());

  std::unique_ptr<Iterator> it = table->NewIterator(ReadOptions());
  it->SeekToFirst();
  // Either first block read fails immediately, or scan encounters corruption
  bool saw_corruption = false;
  while (it->Valid()) {
    it->Next();
  }
  if (!it->status().ok()) {
    EXPECT_TRUE(it->status().IsCorruption()) << it->status().ToString();
    saw_corruption = true;
  }
  EXPECT_TRUE(saw_corruption);
}

class DBTableCorruptionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<uint64_t> counter{0};
    test_dir_ =
        (std::filesystem::temp_directory_path() /
         ("stratadb_corrupt_test_" + std::to_string(::getpid()) + "_" +
          std::to_string(Env::Default()->NowMicros()) + "_" + std::to_string(counter.fetch_add(1))))
            .string();
    std::error_code ec;
    std::filesystem::create_directories(test_dir_, ec);
    ASSERT_FALSE(ec) << ec.message();
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(test_dir_, ec);
  }

  std::string test_dir_;
};

TEST_F(DBTableCorruptionTest, CorruptedSSTableReportsCorruptionNeverWrongData) {
  Options opts;
  opts.create_if_missing = true;
  opts.paranoid_checks = true;
  opts.block_size = 512;

  // 1. Write known keys and flush to produce an L0 SST file
  {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(opts, test_dir_, &db).ok());
    for (int i = 0; i < 100; ++i) {
      char k[32];
      char v[64];
      std::snprintf(k, sizeof(k), "k_%04d", i);
      std::snprintf(v, sizeof(v), "val_%04d_original", i);
      ASSERT_TRUE(db->Put(WriteOptions(), k, v).ok());
    }
    ASSERT_TRUE(db->Flush().ok());
  }

  // 2. Locate the SST file in test_dir_
  std::string sst_path;
  for (const auto& entry : std::filesystem::directory_iterator(test_dir_)) {
    const std::string name = entry.path().filename().string();
    uint64_t number = 0;
    FileType type;
    if (ParseFileName(name, &number, &type) && type == FileType::kTableFile) {
      sst_path = entry.path().string();
      break;
    }
  }
  ASSERT_FALSE(sst_path.empty()) << "No SSTable file found after Flush()";

  // 3. Read SST file and flip bits in a data block
  std::string sst_data;
  {
    std::ifstream in(sst_path, std::ios::binary);
    ASSERT_TRUE(in.is_open());
    sst_data.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  ASSERT_GE(sst_data.size(), 128u);

  // Flip bit in the data block
  FlipBit(&sst_data, 50, 2);

  // Write corrupted data back to SST file
  {
    std::ofstream out(sst_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    out.write(sst_data.data(), static_cast<std::streamsize>(sst_data.size()));
  }

  // 4. Reopen DB
  opts.create_if_missing = false;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(opts, test_dir_, &db).ok());

  // 5. Verify reads: must either return exact original or Status::Corruption.
  // NEVER return wrong or corrupted data.
  int corruption_count = 0;
  for (int i = 0; i < 100; ++i) {
    char k[32];
    char expected_v[64];
    std::snprintf(k, sizeof(k), "k_%04d", i);
    std::snprintf(expected_v, sizeof(expected_v), "val_%04d_original", i);

    std::string val;
    Status s = db->Get(ReadOptions(), k, &val);
    if (s.ok()) {
      EXPECT_EQ(val, expected_v);
    } else {
      EXPECT_TRUE(s.IsCorruption()) << "Unexpected status: " << s.ToString();
      corruption_count++;
    }
  }
  EXPECT_GT(corruption_count, 0) << "Expected at least one key in corrupted block to fail";

  // 6. Verify iterator scan also reports corruption
  std::unique_ptr<Iterator> it = db->NewIterator(ReadOptions());
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
  }
  EXPECT_TRUE(it->status().IsCorruption()) << it->status().ToString();
}

}  // namespace
}  // namespace strata

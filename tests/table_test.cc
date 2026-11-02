#include "table/table.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "db/dbformat.h"
#include "strata/env.h"
#include "strata/iterator.h"
#include "strata/options.h"
#include "strata/status.h"
#include "table/format.h"
#include "table/table_builder.h"
#include "util/random.h"

namespace strata {
namespace {

// A WritableFile that appends to a string, and can be told to start failing.
class StringSink : public WritableFile {
 public:
  Status Append(std::string_view data) override {
    if (fail_after_ >= 0 && appends_ >= fail_after_) {
      return Status::IOError("simulated write failure");
    }
    ++appends_;
    contents_.append(data.data(), data.size());
    return Status::OK();
  }
  Status Flush() override { return Status::OK(); }
  Status Sync() override { return Status::OK(); }
  Status Close() override { return Status::OK(); }

  void FailAfter(int appends) { fail_after_ = appends; }
  const std::string& contents() const { return contents_; }

 private:
  std::string contents_;
  int appends_ = 0;
  int fail_after_ = -1;
};

// A RandomAccessFile over a string that counts reads, so tests can see how
// many blocks a lookup touched.
class StringSource : public RandomAccessFile {
 public:
  explicit StringSource(std::string contents) : contents_(std::move(contents)) {}
  Status Read(uint64_t offset, size_t n, std::string_view* result, char* scratch) const override {
    reads_.fetch_add(1, std::memory_order_relaxed);
    if (offset >= contents_.size()) {
      *result = {};
      return Status::OK();
    }
    const size_t len = std::min(n, static_cast<size_t>(contents_.size() - offset));
    std::memcpy(scratch, contents_.data() + offset, len);
    *result = std::string_view(scratch, len);
    return Status::OK();
  }
  int reads() const { return reads_.load(std::memory_order_relaxed); }
  void ResetReads() { reads_.store(0, std::memory_order_relaxed); }

 private:
  std::string contents_;
  mutable std::atomic<int> reads_{0};
};

struct Entry {
  std::string key;  // internal key
  std::string value;
};

const InternalKeyComparator kIcmp;

std::string UserKey(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%06d", i);
  return buf;
}

// Several versions of `num_user_keys` user keys, some of them tombstones,
// sorted the way a table stores them (user key ascending, newest first).
std::vector<Entry> RandomEntries(Random* rnd, int num_user_keys) {
  std::vector<Entry> entries;
  SequenceNumber seq = 1;
  for (int i = 0; i < num_user_keys; ++i) {
    const int versions = 1 + static_cast<int>(rnd->Uniform(3));
    for (int v = 0; v < versions; ++v) {
      const ValueType type = rnd->OneIn(5) ? kTypeDeletion : kTypeValue;
      std::string value = type == kTypeValue
                              ? std::string(rnd->Skewed(7), 'v') + std::to_string(seq)
                              : std::string();
      entries.push_back({MakeInternalKey(UserKey(i * 2), seq++, type), std::move(value)});
    }
  }
  std::sort(entries.begin(), entries.end(),
            [](const Entry& a, const Entry& b) { return kIcmp.Compare(a.key, b.key) < 0; });
  return entries;
}

Options TableOptions(size_t block_size = 4096, int restart_interval = 16, int bloom_bits = 10) {
  Options options;
  options.block_size = block_size;
  options.block_restart_interval = restart_interval;
  options.bloom_bits_per_key = bloom_bits;
  return options;
}

std::string BuildTable(const Options& options, const std::vector<Entry>& entries) {
  StringSink sink;
  TableBuilder builder(options, &kIcmp, &sink);
  for (const Entry& e : entries) {
    builder.Add(e.key, e.value);
  }
  EXPECT_TRUE(builder.Finish().ok());
  EXPECT_EQ(builder.NumEntries(), entries.size());
  EXPECT_EQ(builder.FileSize(), sink.contents().size());
  return sink.contents();
}

// Opens `contents`; *source is left pointing at the file so reads can be counted.
Status OpenTable(const Options& options, const std::string& contents, std::unique_ptr<Table>* table,
                 StringSource** source = nullptr) {
  auto file = std::make_unique<StringSource>(contents);
  if (source != nullptr) {
    *source = file.get();
  }
  return Table::Open(options, &kIcmp, std::move(file), contents.size(), table);
}

// The index of the first entry >= target, as InternalGet and Seek must find it.
size_t LowerBound(const std::vector<Entry>& entries, std::string_view target) {
  return static_cast<size_t>(std::lower_bound(entries.begin(), entries.end(), target,
                                              [](const Entry& e, std::string_view t) {
                                                return kIcmp.Compare(e.key, t) < 0;
                                              }) -
                             entries.begin());
}

void CheckTableMatchesModel(const Table& table, const std::vector<Entry>& entries, Random* rnd) {
  std::unique_ptr<Iterator> it = table.NewIterator();

  // Forward scan, then backward (Prev crosses data block boundaries).
  it->SeekToFirst();
  for (const Entry& e : entries) {
    ASSERT_TRUE(it->Valid());
    ASSERT_EQ(it->key(), e.key);
    ASSERT_EQ(it->value(), e.value);
    it->Next();
  }
  EXPECT_FALSE(it->Valid());
  it->SeekToLast();
  for (auto rit = entries.rbegin(); rit != entries.rend(); ++rit) {
    ASSERT_TRUE(it->Valid());
    ASSERT_EQ(it->key(), rit->key);
    it->Prev();
  }
  EXPECT_FALSE(it->Valid());
  ASSERT_TRUE(it->status().ok()) << it->status().ToString();

  // Seek and InternalGet to every entry.
  for (const Entry& e : entries) {
    it->Seek(e.key);
    ASSERT_TRUE(it->Valid());
    ASSERT_EQ(it->key(), e.key);
    bool found = false;
    std::string key;
    std::string value;
    ASSERT_TRUE(table.InternalGet(e.key, &found, &key, &value).ok());
    ASSERT_TRUE(found);
    ASSERT_EQ(key, e.key);
    ASSERT_EQ(value, e.value);
  }

  // Lookups the way Get makes them: a user key at some snapshot sequence.
  for (int i = 0; i < 300; ++i) {
    const int n = static_cast<int>(rnd->Uniform(static_cast<uint32_t>(entries.size()) + 10));
    const std::string user_key = UserKey(n);  // odd n: a user key that was never written
    const SequenceNumber snapshot = rnd->Uniform(static_cast<uint32_t>(entries.size()) + 5);
    const LookupKey lookup(user_key, snapshot);
    const size_t expected = LowerBound(entries, lookup.internal_key());

    it->Seek(lookup.internal_key());
    if (expected == entries.size()) {
      EXPECT_FALSE(it->Valid());
    } else {
      ASSERT_TRUE(it->Valid());
      ASSERT_EQ(it->key(), entries[expected].key);
    }

    bool found = false;
    std::string key;
    std::string value;
    ASSERT_TRUE(table.InternalGet(lookup.internal_key(), &found, &key, &value).ok());
    if (expected < entries.size() && ExtractUserKey(entries[expected].key) == user_key) {
      // The answer is in this table: the filter must not hide it.
      ASSERT_TRUE(found) << user_key;
      ASSERT_EQ(key, entries[expected].key);
      ASSERT_EQ(value, entries[expected].value);
    } else if (found) {
      // Allowed (a filter false positive, or the next user key), but it must
      // be exactly the next entry, never something made up.
      ASSERT_LT(expected, entries.size());
      ASSERT_EQ(key, entries[expected].key);
    }
  }
}

TEST(TableTest, EmptyTable) {
  const Options options = TableOptions();
  const std::string contents = BuildTable(options, {});
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(options, contents, &table).ok());
  std::unique_ptr<Iterator> it = table->NewIterator();
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());
  bool found = true;
  std::string key;
  std::string value;
  ASSERT_TRUE(table->InternalGet(MakeInternalKey("x", 1, kTypeValue), &found, &key, &value).ok());
  EXPECT_FALSE(found);
}

TEST(TableTest, RoundTripAgainstModel) {
  for (const size_t block_size : {size_t{64}, size_t{256}, size_t{4096}, size_t{1} << 20}) {
    for (const int restart_interval : {1, 16}) {
      SCOPED_TRACE("block_size=" + std::to_string(block_size) +
                   " restart_interval=" + std::to_string(restart_interval));
      Random rnd(block_size + static_cast<uint64_t>(restart_interval));
      const std::vector<Entry> entries = RandomEntries(&rnd, 500);
      const Options options = TableOptions(block_size, restart_interval);
      const std::string contents = BuildTable(options, entries);
      std::unique_ptr<Table> table;
      ASSERT_TRUE(OpenTable(options, contents, &table).ok());
      EXPECT_TRUE(table->has_filter());
      CheckTableMatchesModel(*table, entries, &rnd);
    }
  }
}

TEST(TableTest, WithoutAFilter) {
  Random rnd(41);
  const std::vector<Entry> entries = RandomEntries(&rnd, 300);
  const Options options = TableOptions(256, 16, /*bloom_bits=*/0);
  const std::string contents = BuildTable(options, entries);
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(options, contents, &table).ok());
  EXPECT_FALSE(table->has_filter());
  CheckTableMatchesModel(*table, entries, &rnd);
}

TEST(TableTest, FilterIsKeyedOnUserKeys) {
  // Entries are written at sequence 100; Get searches at the latest sequence.
  // A filter built from internal keys would answer "definitely not here".
  std::vector<Entry> entries;
  entries.reserve(200);
  for (int i = 0; i < 200; ++i) {
    entries.push_back({MakeInternalKey(UserKey(i), 100, kTypeValue), "v" + std::to_string(i)});
  }
  const Options options = TableOptions();
  const std::string contents = BuildTable(options, entries);
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(options, contents, &table).ok());
  for (int i = 0; i < 200; ++i) {
    const LookupKey lookup(UserKey(i), kMaxSequenceNumber);
    bool found = false;
    std::string key;
    std::string value;
    ASSERT_TRUE(table->InternalGet(lookup.internal_key(), &found, &key, &value).ok());
    ASSERT_TRUE(found) << UserKey(i);
    EXPECT_EQ(ExtractUserKey(key), UserKey(i));
    EXPECT_EQ(value, "v" + std::to_string(i));
  }
}

TEST(TableTest, BloomFilterSkipsDataBlockReads) {
  std::vector<Entry> entries;
  entries.reserve(2000);
  for (int i = 0; i < 2000; ++i) {
    entries.push_back({MakeInternalKey(UserKey(i), 1, kTypeValue), "value"});
  }
  // Each lookup is for a key between two stored keys, so without a filter the
  // index sends every lookup to a data block.
  auto count_reads = [&](int bloom_bits) {
    const Options options = TableOptions(256, 16, bloom_bits);
    const std::string contents = BuildTable(options, entries);
    std::unique_ptr<Table> table;
    StringSource* source = nullptr;
    EXPECT_TRUE(OpenTable(options, contents, &table, &source).ok());
    source->ResetReads();
    for (int i = 0; i < 2000; ++i) {
      const LookupKey lookup(UserKey(i) + "x", kMaxSequenceNumber);
      bool found = false;
      std::string key;
      std::string value;
      EXPECT_TRUE(table->InternalGet(lookup.internal_key(), &found, &key, &value).ok());
    }
    return source->reads();
  };
  const int with_filter = count_reads(10);
  const int without_filter = count_reads(0);
  // Every lookup but the last reads a block; the last sorts after every key,
  // so the index alone rules it out.
  EXPECT_EQ(without_filter, 1999);
  // About a 0.8% false-positive rate: ~16 reads expected; allow some slack.
  EXPECT_LE(with_filter, 60) << "with filter: " << with_filter << " reads";
}

TEST(TableTest, ValueLargerThanABlock) {
  std::vector<Entry> entries = {
      {MakeInternalKey("a", 1, kTypeValue), "small"},
      {MakeInternalKey("b", 2, kTypeValue), std::string(size_t{100} * 1024, 'x')},
      {MakeInternalKey("c", 3, kTypeValue), "small"},
  };
  const Options options = TableOptions(4096);
  const std::string contents = BuildTable(options, entries);
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(options, contents, &table).ok());
  Random rnd(3);
  CheckTableMatchesModel(*table, entries, &rnd);
}

TEST(TableTest, NotAnSSTable) {
  const Options options = TableOptions();
  std::unique_ptr<Table> table;
  for (const std::string& contents :
       {std::string(), std::string(47, 'x'), std::string(48, '\0'), std::string(5000, 'z')}) {
    Status s = OpenTable(options, contents, &table);
    EXPECT_TRUE(s.IsCorruption()) << s.ToString();
    EXPECT_EQ(table, nullptr);
  }
}

TEST(TableTest, TruncatedAtEveryLength) {
  Random rnd(43);
  const std::vector<Entry> entries = RandomEntries(&rnd, 20);
  const Options options = TableOptions(128);
  const std::string contents = BuildTable(options, entries);
  for (size_t len = 0; len < contents.size(); ++len) {
    std::unique_ptr<Table> table;
    Status s = OpenTable(options, contents.substr(0, len), &table);
    // Cutting the file moves the footer, so the magic number check catches it.
    EXPECT_TRUE(s.IsCorruption()) << "len=" << len << " " << s.ToString();
  }
}

TEST(TableTest, EveryCorruptedByteIsDetectedNeverMisread) {
  // Your plan's corruption test: damage any byte, and the table must either
  // report Corruption or return exactly the right data, never wrong data.
  Random rnd(47);
  const std::vector<Entry> entries = RandomEntries(&rnd, 25);
  const Options options = TableOptions(128);  // several data blocks
  const std::string contents = BuildTable(options, entries);

  for (size_t byte = 0; byte < contents.size(); ++byte) {
    SCOPED_TRACE("byte=" + std::to_string(byte));
    std::string corrupted = contents;
    corrupted[byte] = static_cast<char>(static_cast<uint8_t>(corrupted[byte]) ^ 0x5AU);

    std::unique_ptr<Table> table;
    Status s = OpenTable(options, corrupted, &table);
    if (!s.ok()) {
      EXPECT_TRUE(s.IsCorruption()) << s.ToString();
      continue;
    }

    // A full scan returns a prefix-free subset of the real entries, in order,
    // and anything it cannot read is reported as Corruption.
    std::unique_ptr<Iterator> it = table->NewIterator();
    size_t next = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      const size_t pos = LowerBound(entries, it->key());
      ASSERT_LT(pos, entries.size());
      ASSERT_GE(pos, next) << "entries out of order";
      ASSERT_EQ(it->key(), entries[pos].key) << "a key that was never written";
      ASSERT_EQ(it->value(), entries[pos].value) << "a wrong value";
      next = pos + 1;
    }
    EXPECT_TRUE(it->status().ok() || it->status().IsCorruption()) << it->status().ToString();

    // Every point lookup is either right or an error.
    for (const Entry& e : entries) {
      bool found = false;
      std::string key;
      std::string value;
      Status gs = table->InternalGet(e.key, &found, &key, &value);
      if (gs.ok()) {
        ASSERT_TRUE(found) << "a stored entry vanished without an error";
        ASSERT_EQ(key, e.key);
        ASSERT_EQ(value, e.value);
      } else {
        EXPECT_TRUE(gs.IsCorruption()) << gs.ToString();
      }
    }
  }
}

TEST(TableTest, RealFileThroughEnv) {
  Env* env = Env::Default();
  const std::string dir = (std::filesystem::temp_directory_path() /
                           ("stratadb_table_test_" + std::to_string(::getpid()) + "_" +
                            std::to_string(env->NowMicros())))
                              .string();
  ASSERT_TRUE(env->CreateDir(dir).ok());
  const std::string fname = dir + "/000007.sst";

  Random rnd(53);
  const std::vector<Entry> entries = RandomEntries(&rnd, 1000);
  const Options options = TableOptions();
  {
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(env->NewWritableFile(fname, &file).ok());
    TableBuilder builder(options, &kIcmp, file.get());
    for (const Entry& e : entries) {
      builder.Add(e.key, e.value);
    }
    ASSERT_TRUE(builder.Finish().ok());
    // What flush and compaction must do before trusting the table.
    ASSERT_TRUE(file->Sync().ok());
    ASSERT_TRUE(file->Close().ok());
    ASSERT_TRUE(env->SyncDir(dir).ok());
  }

  uint64_t file_size = 0;
  ASSERT_TRUE(env->GetFileSize(fname, &file_size).ok());
  std::unique_ptr<RandomAccessFile> file;
  ASSERT_TRUE(env->NewRandomAccessFile(fname, &file).ok());
  std::unique_ptr<Table> table;
  ASSERT_TRUE(Table::Open(options, &kIcmp, std::move(file), file_size, &table).ok());
  CheckTableMatchesModel(*table, entries, &rnd);

  table.reset();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(TableTest, WriteErrorIsSticky) {
  Random rnd(59);
  const std::vector<Entry> entries = RandomEntries(&rnd, 200);
  StringSink sink;
  sink.FailAfter(3);  // a few blocks succeed, then the disk "fills up"
  TableBuilder builder(TableOptions(128), &kIcmp, &sink);
  for (const Entry& e : entries) {
    builder.Add(e.key, e.value);
  }
  EXPECT_TRUE(builder.status().IsIOError());
  EXPECT_TRUE(builder.Finish().IsIOError());
  EXPECT_LT(builder.FileSize(), 1000U);
}

TEST(TableTest, AbandonWritesNothingMore) {
  StringSink sink;
  TableBuilder builder(TableOptions(), &kIcmp, &sink);
  builder.Add(MakeInternalKey("a", 1, kTypeValue), "v");
  builder.Abandon();
  // The single small block was never flushed, so nothing reached the file.
  EXPECT_TRUE(sink.contents().empty());
}

TEST(TableTest, ConcurrentLookups) {
  Random rnd(61);
  const std::vector<Entry> entries = RandomEntries(&rnd, 1000);
  const Options options = TableOptions(256);
  const std::string contents = BuildTable(options, entries);
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTable(options, contents, &table).ok());

  std::vector<std::thread> threads;
  threads.reserve(4);
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&table, &entries, t]() {
      for (size_t i = static_cast<size_t>(t); i < entries.size(); i += 4) {
        bool found = false;
        std::string key;
        std::string value;
        EXPECT_TRUE(table->InternalGet(entries[i].key, &found, &key, &value).ok());
        EXPECT_TRUE(found);
        EXPECT_EQ(value, entries[i].value);
      }
      std::unique_ptr<Iterator> it = table->NewIterator();
      size_t count = 0;
      for (it->SeekToFirst(); it->Valid(); it->Next()) {
        ++count;
      }
      EXPECT_EQ(count, entries.size());
    });
  }
  for (std::thread& th : threads) {
    th.join();
  }
}

}  // namespace
}  // namespace strata

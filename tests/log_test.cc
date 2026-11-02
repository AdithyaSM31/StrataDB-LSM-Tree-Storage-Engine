#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "log/log_format.h"
#include "log/log_reader.h"
#include "log/log_writer.h"
#include "strata/env.h"
#include "strata/status.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/random.h"

namespace strata::log {
namespace {

// Test Double: WritableFile that appends to a std::string.
class StringSink : public WritableFile {
 public:
  StringSink() : dest_(&internal_contents_) {}
  explicit StringSink(std::string* dest) : dest_(dest) {}
  ~StringSink() override = default;

  Status Append(std::string_view data) override {
    dest_->append(data.data(), data.size());
    return Status::OK();
  }

  Status Flush() override {
    ++flush_count_;
    return Status::OK();
  }

  Status Sync() override {
    ++sync_count_;
    return Status::OK();
  }

  Status Close() override { return Status::OK(); }

  const std::string& contents() const { return *dest_; }
  std::string* mutable_contents() { return dest_; }
  int flush_count() const { return flush_count_; }
  int sync_count() const { return sync_count_; }

 private:
  std::string internal_contents_;
  std::string* dest_;
  int flush_count_ = 0;
  int sync_count_ = 0;
};

// Test Double: SequentialFile that reads from a std::string and can fail reads.
class StringSource : public SequentialFile {
 public:
  explicit StringSource(std::string_view source) : source_(source), offset_(0) {}
  ~StringSource() override = default;

  Status Read(size_t n, std::string_view* result, char* /*scratch*/) override {
    if (fail_read_) {
      return Status::IOError("simulated read failure");
    }
    const size_t available = (offset_ < source_.size()) ? (source_.size() - offset_) : 0;
    const size_t to_read = std::min(n, available);
    *result = std::string_view(source_.data() + offset_, to_read);
    offset_ += to_read;
    return Status::OK();
  }

  Status Skip(uint64_t n) override {
    if (fail_read_) {
      return Status::IOError("simulated skip failure");
    }
    const size_t available = (offset_ < source_.size()) ? (source_.size() - offset_) : 0;
    const size_t to_skip = std::min(static_cast<size_t>(n), available);
    offset_ += to_skip;
    return Status::OK();
  }

  void set_fail_read(bool fail) { fail_read_ = fail; }
  size_t offset() const { return offset_; }

 private:
  std::string_view source_;
  size_t offset_ = 0;
  bool fail_read_ = false;
};

// Test Double: Reporter that collects dropped bytes and corruption statuses.
// Flip one bit of s[byte], XOR-ing on uint8_t to avoid an int -> char narrowing.
void FlipBit(std::string& s, size_t byte, size_t bit) {
  s[byte] = static_cast<char>(static_cast<uint8_t>(s[byte]) ^ (1U << bit));
}

class TestReporter : public Reader::Reporter {
 public:
  struct Report {
    size_t bytes;
    Status status;
  };

  void Corruption(size_t bytes, const Status& status) override {
    reports.push_back({bytes, status});
  }

  void Clear() { reports.clear(); }

  std::vector<Report> reports;
};

// Helper to read all records from a string source using Reader.
std::vector<std::string> ReadAllRecords(std::string_view log_data, TestReporter* reporter = nullptr,
                                        bool checksum = true) {
  StringSource src(log_data);
  Reader reader(&src, reporter, checksum);
  std::vector<std::string> result;
  std::string_view record;
  std::string scratch;
  while (reader.ReadRecord(&record, &scratch)) {
    result.emplace_back(record.data(), record.size());
  }
  return result;
}

TEST(LogTest, EmptyLog) {
  TestReporter reporter;
  auto records = ReadAllRecords("", &reporter);
  EXPECT_TRUE(records.empty());
  EXPECT_TRUE(reporter.reports.empty());
}

TEST(LogTest, EmptyRecord) {
  StringSink sink;
  Writer writer(&sink);
  ASSERT_TRUE(writer.AddRecord("").ok());
  EXPECT_GE(sink.flush_count(), 1);

  TestReporter reporter;
  auto records = ReadAllRecords(sink.contents(), &reporter);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0], "");
  EXPECT_TRUE(reporter.reports.empty());
}

TEST(LogTest, SmallRecords) {
  StringSink sink;
  Writer writer(&sink);
  ASSERT_TRUE(writer.AddRecord("hello").ok());
  ASSERT_TRUE(writer.AddRecord("world").ok());
  ASSERT_TRUE(writer.AddRecord("stratadb").ok());

  TestReporter reporter;
  auto records = ReadAllRecords(sink.contents(), &reporter);
  ASSERT_EQ(records.size(), 3u);
  EXPECT_EQ(records[0], "hello");
  EXPECT_EQ(records[1], "world");
  EXPECT_EQ(records[2], "stratadb");
  EXPECT_TRUE(reporter.reports.empty());
}

TEST(LogTest, ExactlyFillsBlock) {
  // A record that exactly fills one 32 KB block:
  // kBlockSize = 32768, kHeaderSize = 7 -> payload size = 32761.
  const size_t fill_size = kBlockSize - kHeaderSize;
  const std::string large(fill_size, 'X');

  StringSink sink;
  Writer writer(&sink);
  ASSERT_TRUE(writer.AddRecord(large).ok());
  EXPECT_EQ(sink.contents().size(), kBlockSize);

  TestReporter reporter;
  auto records = ReadAllRecords(sink.contents(), &reporter);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0], large);
  EXPECT_TRUE(reporter.reports.empty());
}

TEST(LogTest, MultiBlockRecord100KB) {
  // 100 KB record spanning 4 blocks.
  const std::string huge(size_t{100} * 1024, 'M');

  StringSink sink;
  Writer writer(&sink);
  ASSERT_TRUE(writer.AddRecord(huge).ok());

  TestReporter reporter;
  auto records = ReadAllRecords(sink.contents(), &reporter);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0], huge);
  EXPECT_TRUE(reporter.reports.empty());
}

TEST(LogTest, ExactlySevenBytesLeftEmitsZeroLengthFirstPiece) {
  // When exactly 7 bytes are left in a block, the writer must emit
  // a zero-length First piece, then continue in the next block.
  // Record 1 size: (kBlockSize - 7) - kHeaderSize = 32768 - 7 - 7 = 32754 bytes.
  const size_t rec1_size = kBlockSize - 2 * kHeaderSize;
  const std::string rec1(rec1_size, 'A');
  const std::string rec2 = "continuation_in_next_block";

  StringSink sink;
  Writer writer(&sink);
  ASSERT_TRUE(writer.AddRecord(rec1).ok());
  // After rec1, exactly 7 bytes remain in the 32 KB block.
  EXPECT_EQ(sink.contents().size(), kBlockSize - kHeaderSize);

  ASSERT_TRUE(writer.AddRecord(rec2).ok());
  // The zero-length First piece took 7 bytes (filling block 0 to 32768).
  // Block 1 then contains rec2 as kLastType (7 + rec2.size()).
  EXPECT_EQ(sink.contents().size(), kBlockSize + kHeaderSize + rec2.size());

  TestReporter reporter;
  auto records = ReadAllRecords(sink.contents(), &reporter);
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0], rec1);
  EXPECT_EQ(records[1], rec2);
  EXPECT_TRUE(reporter.reports.empty());
}

TEST(LogTest, SixBytesLeftPadsWithZeros) {
  // When 6 bytes are left, the writer cannot fit a 7-byte header,
  // so it must append 6 zero bytes to trailer and start the new block.
  // Record 1 size: 32768 - 6 - 7 = 32755 bytes.
  const size_t rec1_size = kBlockSize - 6 - kHeaderSize;
  const std::string rec1(rec1_size, 'B');
  const std::string rec2 = "fresh_block_record";

  StringSink sink;
  Writer writer(&sink);
  ASSERT_TRUE(writer.AddRecord(rec1).ok());
  EXPECT_EQ(sink.contents().size(), kBlockSize - 6);

  ASSERT_TRUE(writer.AddRecord(rec2).ok());
  // 6 bytes trailer + 7 bytes header + rec2.size()
  EXPECT_EQ(sink.contents().size(), kBlockSize + kHeaderSize + rec2.size());

  TestReporter reporter;
  auto records = ReadAllRecords(sink.contents(), &reporter);
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0], rec1);
  EXPECT_EQ(records[1], rec2);
  EXPECT_TRUE(reporter.reports.empty());
}

TEST(LogTest, ThousandRandomRecords) {
  Random rng(1337);
  std::vector<std::string> expected;
  expected.reserve(1000);

  StringSink sink;
  Writer writer(&sink);

  for (int i = 0; i < 1000; ++i) {
    // Generate records mostly small with occasional large ones up to ~64 KB
    const uint32_t len = rng.Skewed(16);
    std::string data(len, '\0');
    for (uint32_t j = 0; j < len; ++j) {
      data[j] = static_cast<char>('a' + (rng.Next() % 26));
    }
    ASSERT_TRUE(writer.AddRecord(data).ok());
    expected.push_back(std::move(data));
  }

  TestReporter reporter;
  auto records = ReadAllRecords(sink.contents(), &reporter);
  ASSERT_EQ(records.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    ASSERT_EQ(records[i], expected[i]) << "Mismatch at index " << i;
  }
  EXPECT_TRUE(reporter.reports.empty());
}

TEST(LogTest, ReopenWriter) {
  StringSink sink;
  {
    Writer w1(&sink);
    ASSERT_TRUE(w1.AddRecord("entry1").ok());
    ASSERT_TRUE(w1.AddRecord("entry2").ok());
  }

  const uint64_t dest_length = sink.contents().size();

  {
    Writer w2(&sink, dest_length);
    ASSERT_TRUE(w2.AddRecord("entry3").ok());
    ASSERT_TRUE(w2.AddRecord("entry4").ok());
  }

  TestReporter reporter;
  auto records = ReadAllRecords(sink.contents(), &reporter);
  ASSERT_EQ(records.size(), 4u);
  EXPECT_EQ(records[0], "entry1");
  EXPECT_EQ(records[1], "entry2");
  EXPECT_EQ(records[2], "entry3");
  EXPECT_EQ(records[3], "entry4");
  EXPECT_TRUE(reporter.reports.empty());
}

TEST(LogTest, TruncationTornTailAtEveryBytePosition) {
  StringSink sink;
  Writer writer(&sink);
  ASSERT_TRUE(writer.AddRecord("record0_small").ok());
  ASSERT_TRUE(writer.AddRecord("record1_medium_data_here").ok());
  ASSERT_TRUE(writer.AddRecord("record2_another_good_record").ok());

  const size_t prefix_len = sink.contents().size();

  // Add the last record (multi-block to test cut points across fragments too)
  const std::string last_record(40000, 'Z');
  ASSERT_TRUE(writer.AddRecord(last_record).ok());

  const std::string full_log = sink.contents();
  const size_t total_len = full_log.size();

  // Cut the file at EVERY byte position within the last record:
  for (size_t cut_len = prefix_len; cut_len < total_len; ++cut_len) {
    const std::string truncated = full_log.substr(0, cut_len);
    TestReporter reporter;
    auto records = ReadAllRecords(truncated, &reporter);

    // Must return all earlier records unchanged
    ASSERT_EQ(records.size(), 3u) << "Failed at cut_len " << cut_len;
    EXPECT_EQ(records[0], "record0_small");
    EXPECT_EQ(records[1], "record1_medium_data_here");
    EXPECT_EQ(records[2], "record2_another_good_record");

    // Must report nothing, because it is a torn tail!
    EXPECT_TRUE(reporter.reports.empty())
        << "Torn tail should not report corruption at cut_len " << cut_len;
  }
}

TEST(LogTest, BitFlipsReportCorruptionAndNeverReturnDamagedRecord) {
  StringSink sink;
  Writer writer(&sink);
  ASSERT_TRUE(writer.AddRecord("prefix_record").ok());

  const size_t r1_start = sink.contents().size();
  const std::string r1_target = "target_record_for_bit_flips";
  ASSERT_TRUE(writer.AddRecord(r1_target).ok());
  const size_t r1_end = sink.contents().size();

  // Ensure there is another block after r1 so r1 is strictly in the middle of the log.
  // In the last block at EOF, an inflated length is indistinguishable from a torn write.
  const std::string large_suffix(kBlockSize, 'S');
  ASSERT_TRUE(writer.AddRecord(large_suffix).ok());

  const std::string original = sink.contents();

  // Flip every single bit of the second record, one by one.
  for (size_t byte_idx = r1_start; byte_idx < r1_end; ++byte_idx) {
    for (int bit = 0; bit < 8; ++bit) {
      std::string corrupted = original;
      FlipBit(corrupted, byte_idx, bit);

      TestReporter reporter;
      auto records = ReadAllRecords(corrupted, &reporter);

      // 1. Must NEVER return the changed record
      for (const auto& rec : records) {
        EXPECT_NE(rec, r1_target) << "Returned corrupted record on bit flip at byte " << byte_idx
                                  << " bit " << bit;
        // Any record it DOES return must be an exact original
        EXPECT_TRUE(rec == "prefix_record" || rec == large_suffix)
            << "Returned non-original record: " << rec;
      }

      // 2. Must report corruption
      EXPECT_FALSE(reporter.reports.empty())
          << "Expected corruption report on bit flip at byte " << byte_idx << " bit " << bit;
    }
  }
}

TEST(LogTest, DamageInMiddleBadLength) {
  StringSink sink;
  Writer writer(&sink);

  // Write record A in block 0
  const std::string recA(20000, 'A');
  ASSERT_TRUE(writer.AddRecord(recA).ok());

  // Pad to next block boundary so record B starts cleanly at block 1
  const size_t rem_block0 = kBlockSize - (sink.contents().size() % kBlockSize);
  const size_t pad_size = rem_block0 - kHeaderSize;
  const std::string pad(pad_size, 'P');
  ASSERT_TRUE(writer.AddRecord(pad).ok());
  ASSERT_EQ(sink.contents().size(), kBlockSize);

  // Record B in block 1
  const std::string recB(5000, 'B');
  ASSERT_TRUE(writer.AddRecord(recB).ok());

  // Pad block 1 so record C starts at block 2
  const size_t rem_block1 = kBlockSize - (sink.contents().size() % kBlockSize);
  const size_t pad1_size = rem_block1 - kHeaderSize;
  const std::string pad1(pad1_size, 'Q');
  ASSERT_TRUE(writer.AddRecord(pad1).ok());
  ASSERT_EQ(sink.contents().size(), 2 * kBlockSize);

  // Record C in block 2
  const std::string recC(1000, 'C');
  ASSERT_TRUE(writer.AddRecord(recC).ok());

  std::string corrupted = sink.contents();
  // Corrupt record B's length field in block 1:
  // In block 1, header bytes: 0..3 CRC, 4..5 length.
  // Set length to 0xffff (exceeds block remaining size).
  corrupted[kBlockSize + 4] = static_cast<char>(0xff);
  corrupted[kBlockSize + 5] = static_cast<char>(0xff);

  TestReporter reporter;
  auto records = ReadAllRecords(corrupted, &reporter);

  // Reader drops rest of block 1, recovers at block 2!
  // Records A, Pad, and Pad1 were before block 1 / in block 1.
  // Record C in block 2 must be successfully read!
  ASSERT_GE(records.size(), 2u);
  EXPECT_EQ(records[0], recA);
  EXPECT_EQ(records.back(), recC);

  // Reported bad record length
  ASSERT_FALSE(reporter.reports.empty());
  bool found_bad_len = false;
  for (const auto& rep : reporter.reports) {
    if (rep.status.message().find("bad record length") != std::string::npos) {
      found_bad_len = true;
      break;
    }
  }
  EXPECT_TRUE(found_bad_len);
}

TEST(LogTest, DamageInMiddleMissingFirstPiece) {
  // A multi-block record: block 0 has kFirstType, block 1 has kLastType.
  const std::string multi(40000, 'X');
  StringSink sink;
  Writer writer(&sink);
  ASSERT_TRUE(writer.AddRecord(multi).ok());

  std::string corrupted = sink.contents();
  // Change kFirstType (offset 6) to kMiddleType
  ASSERT_EQ(static_cast<uint8_t>(corrupted[6]), kFirstType);
  corrupted[6] = static_cast<char>(kMiddleType);

  // Recompute CRC so it passes the CRC check and tests the state machine transition
  const uint32_t len = static_cast<uint8_t>(corrupted[4]) |
                       (static_cast<uint32_t>(static_cast<uint8_t>(corrupted[5])) << 8);
  const uint32_t new_crc = crc32c::Mask(crc32c::Value(corrupted.data() + 6, 1 + len));
  EncodeFixed32(&corrupted[0], new_crc);

  TestReporter reporter;
  auto records = ReadAllRecords(corrupted, &reporter);
  EXPECT_TRUE(records.empty());

  bool found_missing_start = false;
  for (const auto& rep : reporter.reports) {
    if (rep.status.message().find("missing start of fragmented record") != std::string::npos) {
      found_missing_start = true;
      break;
    }
  }
  EXPECT_TRUE(found_missing_start);
}

TEST(LogTest, DamageInMiddleMissingLastPiece) {
  // A multi-block record: block 0 has kFirstType, block 1 has kLastType.
  const std::string multi(40000, 'Y');
  StringSink sink;
  Writer writer(&sink);
  ASSERT_TRUE(writer.AddRecord(multi).ok());

  std::string corrupted = sink.contents();
  // Change kLastType in block 1 (offset kBlockSize + 6) to kFirstType
  ASSERT_EQ(static_cast<uint8_t>(corrupted[kBlockSize + 6]), kLastType);
  corrupted[kBlockSize + 6] = static_cast<char>(kFirstType);

  // Recompute CRC for the modified piece
  const uint32_t len =
      static_cast<uint8_t>(corrupted[kBlockSize + 4]) |
      (static_cast<uint32_t>(static_cast<uint8_t>(corrupted[kBlockSize + 5])) << 8);
  const uint32_t new_crc = crc32c::Mask(crc32c::Value(corrupted.data() + kBlockSize + 6, 1 + len));
  EncodeFixed32(&corrupted[kBlockSize], new_crc);

  TestReporter reporter;
  auto records = ReadAllRecords(corrupted, &reporter);

  bool found_partial_no_end = false;
  for (const auto& rep : reporter.reports) {
    if (rep.status.message().find("partial record without end") != std::string::npos) {
      found_partial_no_end = true;
      break;
    }
  }
  EXPECT_TRUE(found_partial_no_end);
}

TEST(LogTest, ZeroFilledTail) {
  StringSink sink;
  Writer writer(&sink);
  ASSERT_TRUE(writer.AddRecord("hello").ok());
  ASSERT_TRUE(writer.AddRecord("world").ok());

  // Append a zero-filled tail (e.g. crash after preallocation)
  std::string log_data = sink.contents();
  log_data.append(1000, '\0');

  TestReporter reporter;
  auto records = ReadAllRecords(log_data, &reporter);
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0], "hello");
  EXPECT_EQ(records[1], "world");
  EXPECT_TRUE(reporter.reports.empty());

  // Also test an entire 32 KB block of zeroes
  log_data.append(kBlockSize, '\0');
  reporter.Clear();
  records = ReadAllRecords(log_data, &reporter);
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0], "hello");
  EXPECT_EQ(records[1], "world");
  EXPECT_TRUE(reporter.reports.empty());
}

TEST(LogTest, ZeroedMiddleBlockIsReportedAsHole) {
  // One record exactly fills each of three blocks. Zeroing block 1 simulates
  // data that never reached the disk while later data did. Returning A and C
  // silently would leave a hole the DB could not detect, so the reader must
  // report it.
  StringSink sink;
  Writer writer(&sink);
  const std::string a(kBlockSize - kHeaderSize, 'A');
  const std::string b(kBlockSize - kHeaderSize, 'B');
  const std::string c(kBlockSize - kHeaderSize, 'C');
  ASSERT_TRUE(writer.AddRecord(a).ok());
  ASSERT_TRUE(writer.AddRecord(b).ok());
  ASSERT_TRUE(writer.AddRecord(c).ok());
  std::string log_data = sink.contents();
  ASSERT_EQ(log_data.size(), 3 * kBlockSize);
  std::fill(log_data.begin() + kBlockSize, log_data.begin() + 2 * kBlockSize, '\0');

  TestReporter reporter;
  auto records = ReadAllRecords(log_data, &reporter);
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0], a);
  EXPECT_EQ(records[1], c);
  ASSERT_EQ(reporter.reports.size(), 1u);
  EXPECT_TRUE(reporter.reports[0].status.IsCorruption());
  EXPECT_EQ(reporter.reports[0].bytes, kBlockSize);
}

TEST(LogTest, SeveralZeroedBlocksBeforeValidRecordReportedOnce) {
  StringSink sink;
  Writer writer(&sink);
  const std::string rec(kBlockSize - kHeaderSize, 'R');
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(writer.AddRecord(rec).ok());
  }
  std::string log_data = sink.contents();
  std::fill(log_data.begin() + kBlockSize, log_data.begin() + 3 * kBlockSize, '\0');

  TestReporter reporter;
  auto records = ReadAllRecords(log_data, &reporter);
  EXPECT_EQ(records.size(), 2u);
  ASSERT_EQ(reporter.reports.size(), 1u);
  EXPECT_EQ(reporter.reports[0].bytes, 2 * kBlockSize);
}

TEST(LogTest, FragmentedRecordCutOffByZeroFilledTailIsSilent) {
  // The writer finished the First piece of a large record, then crashed; the
  // rest of the file is zeros. That is a torn tail: the earlier record comes
  // back, the partial one does not, and nothing is reported.
  StringSink sink;
  Writer writer(&sink);
  ASSERT_TRUE(writer.AddRecord("before").ok());
  const size_t big_start = sink.contents().size();
  ASSERT_TRUE(writer.AddRecord(std::string(3 * kBlockSize, 'X')).ok());
  std::string log_data = sink.contents();
  // Keep the first block (holding "before" and the First piece); zero the rest.
  ASSERT_LT(big_start, kBlockSize);
  std::fill(log_data.begin() + kBlockSize, log_data.end(), '\0');

  TestReporter reporter;
  auto records = ReadAllRecords(log_data, &reporter);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0], "before");
  EXPECT_TRUE(reporter.reports.empty());
}

TEST(LogTest, ReadErrorFromSourceIsReportedAndStops) {
  StringSink sink;
  Writer writer(&sink);
  ASSERT_TRUE(writer.AddRecord("good_record").ok());

  StringSource src(sink.contents());
  src.set_fail_read(true);

  TestReporter reporter;
  Reader reader(&src, &reporter, true);

  std::string_view rec;
  std::string scratch;
  EXPECT_FALSE(reader.ReadRecord(&rec, &scratch));
  ASSERT_EQ(reporter.reports.size(), 1u);
  EXPECT_TRUE(reporter.reports[0].status.IsIOError());
}

TEST(LogTest, RandomGarbageDoesNotCrash) {
  Random rng(42);
  std::string garbage(100000, '\0');
  for (size_t i = 0; i < garbage.size(); ++i) {
    garbage[i] = static_cast<char>(rng.Next() & 0xff);
  }

  StringSource src(garbage);
  TestReporter reporter;
  Reader reader(&src, &reporter, true);

  std::string_view record;
  std::string scratch;
  while (reader.ReadRecord(&record, &scratch)) {
    // We don't care what random bytes match by sheer chance,
    // only that ASan/UBSan prove no memory corruption occurs.
  }
}

TEST(LogTest, ChecksumDisabledAllowsCorruptedRecord) {
  StringSink sink;
  Writer writer(&sink);
  ASSERT_TRUE(writer.AddRecord("hello_world_data").ok());

  std::string corrupted = sink.contents();
  // Flip a bit in the payload (byte 7 onwards)
  corrupted[8] ^= 0x01;

  // With checksum = false, the reader returns the record despite bit corruption
  TestReporter rep1;
  auto records1 = ReadAllRecords(corrupted, &rep1, /*checksum=*/false);
  ASSERT_EQ(records1.size(), 1u);
  EXPECT_NE(records1[0], "hello_world_data");
  EXPECT_TRUE(rep1.reports.empty());

  // With checksum = true, the reader detects corruption and reports it
  TestReporter rep2;
  auto records2 = ReadAllRecords(corrupted, &rep2, /*checksum=*/true);
  EXPECT_TRUE(records2.empty());
  ASSERT_FALSE(rep2.reports.empty());
  EXPECT_TRUE(rep2.reports[0].status.message().find("checksum mismatch") != std::string::npos);
}

TEST(LogTest, MultiBlockRecordChecksumMismatchInSecondBlock) {
  StringSink sink;
  Writer writer(&sink);

  // Multi-block record (40 KB): block 0 has First fragment, block 1 has Last fragment
  const std::string multi(40000, 'Z');
  ASSERT_TRUE(writer.AddRecord(multi).ok());
  ASSERT_GT(sink.contents().size(), kBlockSize);

  // Pad the rest of block 1 to 2 * kBlockSize so independent_record is cleanly in block 2
  const size_t rem_block1 = (2 * kBlockSize) - sink.contents().size();
  const size_t pad_size = rem_block1 - kHeaderSize;
  const std::string pad(pad_size, 'P');
  ASSERT_TRUE(writer.AddRecord(pad).ok());
  ASSERT_EQ(sink.contents().size(), 2 * kBlockSize);

  // Independent record in block 2
  ASSERT_TRUE(writer.AddRecord("independent_record").ok());

  std::string corrupted = sink.contents();
  // Corrupt a byte in block 1 payload (offset kBlockSize + 8)
  corrupted[kBlockSize + 8] ^= 0x42;

  TestReporter reporter;
  auto records = ReadAllRecords(corrupted, &reporter);

  // The corrupted multi-block record and pad in block 1 should be dropped
  // The independent record in block 2 should survive cleanly!
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0], "independent_record");

  // Corruption must be reported
  ASSERT_FALSE(reporter.reports.empty());
}

TEST(LogTest, AddRecordNullDataEmptyView) {
  StringSink sink;
  Writer writer(&sink);
  // Default constructed string_view has data() == nullptr and size() == 0
  std::string_view null_view;
  ASSERT_TRUE(writer.AddRecord(null_view).ok());

  TestReporter reporter;
  auto records = ReadAllRecords(sink.contents(), &reporter);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0], "");
  EXPECT_TRUE(reporter.reports.empty());
}

TEST(LogTest, TornTailSingleBlockByteByByte) {
  StringSink sink;
  Writer writer(&sink);
  const std::string rec(100, 'T');
  ASSERT_TRUE(writer.AddRecord(rec).ok());

  const std::string full = sink.contents();
  ASSERT_EQ(full.size(), 107u);

  for (size_t cut = 0; cut < full.size(); ++cut) {
    TestReporter reporter;
    auto records = ReadAllRecords(full.substr(0, cut), &reporter);
    EXPECT_TRUE(records.empty()) << "Expected 0 records at cut " << cut;
    EXPECT_TRUE(reporter.reports.empty())
        << "Torn tail at cut " << cut << " should not report corruption";
  }
}

}  // namespace
}  // namespace strata::log

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "db/dbformat.h"
#include "strata/env.h"
#include "strata/iterator.h"
#include "strata/options.h"
#include "strata/status.h"
#include "table/block.h"
#include "table/format.h"
#include "table/table_builder.h"
#include "util/comparator.h"

namespace strata {
namespace {

// tests/CMakeLists.txt passes the exact path of the sst_dump it built (and
// builds it before this test). Empty when tools are disabled.
#ifndef STRATA_SST_DUMP_PATH
#define STRATA_SST_DUMP_PATH ""
#endif

std::string FindSstDumpBinary() { return STRATA_SST_DUMP_PATH; }

struct CommandResult {
  int exit_code = -1;
  std::string output;
};

CommandResult RunSstDump(const std::vector<std::string>& args) {
  const std::string bin = FindSstDumpBinary();
  int pipefd[2];
  if (::pipe(pipefd) != 0) {
    return {-1, "pipe failed"};
  }
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::close(pipefd[0]);
    ::dup2(pipefd[1], STDOUT_FILENO);
    ::dup2(pipefd[1], STDERR_FILENO);
    ::close(pipefd[1]);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(bin.c_str()));
    for (const auto& a : args) {
      argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(bin.c_str(), argv.data());
    ::_exit(127);
  }
  ::close(pipefd[1]);
  std::string out;
  char buf[256];
  ssize_t n = 0;
  while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0) {
    out.append(buf, static_cast<size_t>(n));
  }
  ::close(pipefd[0]);
  int status = 0;
  ::waitpid(pid, &status, 0);
  const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return {exit_code, out};
}

class SstDumpTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (FindSstDumpBinary().empty()) {
      GTEST_SKIP() << "sst_dump was not built (STRATA_BUILD_TOOLS=OFF)";
    }
    test_dir_ = "/tmp/strata_sst_dump_test_" + std::to_string(::getpid());
    std::filesystem::create_directories(test_dir_);
  }

  void TearDown() override {
    if (!test_dir_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(test_dir_, ec);
    }
  }

  std::string MakeTableFile(const std::string& name, int bloom_bits,
                            const std::vector<std::pair<std::string, std::string>>& entries) {
    const std::string path = test_dir_ + "/" + name;
    Env* env = Env::Default();
    std::unique_ptr<WritableFile> file;
    EXPECT_TRUE(env->NewWritableFile(path, &file).ok());

    Options options;
    options.bloom_bits_per_key = bloom_bits;
    InternalKeyComparator icmp;
    TableBuilder builder(options, &icmp, file.get());

    for (const auto& [k, v] : entries) {
      builder.Add(k, v);
    }
    EXPECT_TRUE(builder.Finish().ok());
    EXPECT_TRUE(file->Sync().ok());
    EXPECT_TRUE(file->Close().ok());
    return path;
  }

  std::string test_dir_;
};

TEST_F(SstDumpTest, HelpOption) {
  CommandResult res = RunSstDump({"--help"});
  EXPECT_EQ(res.exit_code, 0);
  EXPECT_NE(res.output.find("Usage:"), std::string::npos);
  EXPECT_NE(res.output.find("--verify"), std::string::npos);
}

TEST_F(SstDumpTest, NoArgumentsFails) {
  CommandResult res = RunSstDump({});
  EXPECT_EQ(res.exit_code, 1);
  EXPECT_NE(res.output.find("No SSTable files specified"), std::string::npos);
}

TEST_F(SstDumpTest, UnknownOptionFails) {
  CommandResult res = RunSstDump({"--nonexistent-flag"});
  EXPECT_EQ(res.exit_code, 1);
  EXPECT_NE(res.output.find("Unknown option"), std::string::npos);
}

TEST_F(SstDumpTest, EmptyFileFails) {
  const std::string empty_path = test_dir_ + "/empty.sst";
  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(Env::Default()->NewWritableFile(empty_path, &file).ok());
  ASSERT_TRUE(file->Close().ok());

  CommandResult res = RunSstDump({empty_path});
  EXPECT_EQ(res.exit_code, 1);
  EXPECT_NE(res.output.find("Corruption"), std::string::npos);
}

TEST_F(SstDumpTest, ValidTableDumpEntries) {
  std::vector<std::pair<std::string, std::string>> entries = {
      {MakeInternalKey("apple", 10, kTypeValue), "red"},
      {MakeInternalKey("banana", 8, kTypeValue), "yellow"},
      {MakeInternalKey("cherry", 15, kTypeDeletion), ""},
  };
  std::string path = MakeTableFile("test.sst", 10, entries);

  CommandResult res = RunSstDump({path});
  EXPECT_EQ(res.exit_code, 0);
  EXPECT_NE(res.output.find("metaindex handle: offset "), std::string::npos);
  EXPECT_NE(res.output.find("index handle: offset "), std::string::npos);
  EXPECT_NE(res.output.find("filter: present"), std::string::npos);
  EXPECT_NE(res.output.find("apple @ 10 : val => red"), std::string::npos);
  EXPECT_NE(res.output.find("banana @ 8 : val => yellow"), std::string::npos);
  EXPECT_NE(res.output.find("cherry @ 15 : del => "), std::string::npos);
}

TEST_F(SstDumpTest, TableWithoutFilter) {
  std::vector<std::pair<std::string, std::string>> entries = {
      {MakeInternalKey("k1", 1, kTypeValue), "v1"},
  };
  std::string path = MakeTableFile("nofilter.sst", 0, entries);

  CommandResult res = RunSstDump({path});
  EXPECT_EQ(res.exit_code, 0);
  EXPECT_NE(res.output.find("filter: none"), std::string::npos);
  EXPECT_NE(res.output.find("k1 @ 1 : val => v1"), std::string::npos);
}

TEST_F(SstDumpTest, ValidTableVerify) {
  std::vector<std::pair<std::string, std::string>> entries = {
      {MakeInternalKey("apple", 10, kTypeValue), "red"},
      {MakeInternalKey("banana", 8, kTypeValue), "yellow"},
      {MakeInternalKey("cherry", 15, kTypeDeletion), ""},
  };
  std::string path = MakeTableFile("test.sst", 10, entries);

  CommandResult res = RunSstDump({"--verify", path});
  EXPECT_EQ(res.exit_code, 0);
  EXPECT_NE(res.output.find("metaindex handle: offset "), std::string::npos);
  EXPECT_NE(res.output.find("index handle: offset "), std::string::npos);
  EXPECT_NE(res.output.find("filter: present"), std::string::npos);
  EXPECT_NE(res.output.find("OK: verified 3 entries across 1 data blocks"), std::string::npos);
  // Default --verify does not dump entries.
  EXPECT_EQ(res.output.find("apple @ 10 : val => red"), std::string::npos);
}

TEST_F(SstDumpTest, VerifyAndShowEntries) {
  std::vector<std::pair<std::string, std::string>> entries = {
      {MakeInternalKey("apple", 10, kTypeValue), "red"},
  };
  std::string path = MakeTableFile("test.sst", 10, entries);

  CommandResult res = RunSstDump({"--verify", "--show-entries", path});
  EXPECT_EQ(res.exit_code, 0);
  EXPECT_NE(res.output.find("apple @ 10 : val => red"), std::string::npos);
  EXPECT_NE(res.output.find("OK: verified 1 entries across 1 data blocks"), std::string::npos);
}

TEST_F(SstDumpTest, CorruptedFooterDetected) {
  std::vector<std::pair<std::string, std::string>> entries = {
      {MakeInternalKey("k1", 1, kTypeValue), "v1"},
  };
  std::string path = MakeTableFile("corrupt_footer.sst", 10, entries);

  // Flip a byte in the footer (last 48 bytes).
  FILE* f = std::fopen(path.c_str(), "r+b");
  ASSERT_NE(f, nullptr);
  ASSERT_EQ(std::fseek(f, -5, SEEK_END), 0);
  int c = std::fgetc(f);
  ASSERT_EQ(std::fseek(f, -5, SEEK_END), 0);
  std::fputc(c ^ 0xFF, f);
  std::fclose(f);

  CommandResult res = RunSstDump({path});
  EXPECT_EQ(res.exit_code, 1);
  EXPECT_NE(res.output.find("Corruption"), std::string::npos);

  CommandResult verify_res = RunSstDump({"--verify", path});
  EXPECT_EQ(verify_res.exit_code, 1);
  EXPECT_NE(verify_res.output.find("Corruption"), std::string::npos);
}

TEST_F(SstDumpTest, CorruptedDataBlockDetectedByVerify) {
  std::vector<std::pair<std::string, std::string>> entries = {
      {MakeInternalKey("alpha", 10, kTypeValue), "v1"},
      {MakeInternalKey("bravo", 20, kTypeValue), "v2"},
  };
  std::string path = MakeTableFile("corrupt_data.sst", 10, entries);

  // Flip byte 10 (inside the first data block).
  FILE* f = std::fopen(path.c_str(), "r+b");
  ASSERT_NE(f, nullptr);
  ASSERT_EQ(std::fseek(f, 10, SEEK_SET), 0);
  int c = std::fgetc(f);
  ASSERT_EQ(std::fseek(f, 10, SEEK_SET), 0);
  std::fputc(c ^ 0x01, f);
  std::fclose(f);

  CommandResult verify_res = RunSstDump({"--verify", path});
  EXPECT_EQ(verify_res.exit_code, 1);
  EXPECT_NE(verify_res.output.find("Corruption"), std::string::npos);
}

TEST_F(SstDumpTest, CorruptedFilterBlockDetectedByVerify) {
  std::vector<std::pair<std::string, std::string>> entries = {
      {MakeInternalKey("alpha", 10, kTypeValue), "v1"},
      {MakeInternalKey("bravo", 20, kTypeValue), "v2"},
  };
  std::string path = MakeTableFile("corrupt_filter.sst", 10, entries);

  Env* env = Env::Default();
  uint64_t file_size = 0;
  ASSERT_TRUE(env->GetFileSize(path, &file_size).ok());
  std::unique_ptr<RandomAccessFile> file;
  ASSERT_TRUE(env->NewRandomAccessFile(path, &file).ok());
  std::string footer_space(Footer::kEncodedLength, '\0');
  std::string_view footer_input;
  ASSERT_TRUE(file->Read(file_size - Footer::kEncodedLength, Footer::kEncodedLength, &footer_input,
                         footer_space.data())
                  .ok());
  Footer footer;
  ASSERT_TRUE(footer.DecodeFrom(footer_input).ok());

  std::string metaindex_contents;
  ASSERT_TRUE(
      ReadBlock(file.get(), file_size, footer.metaindex_handle(), &metaindex_contents).ok());
  Block metaindex(std::move(metaindex_contents));
  std::unique_ptr<Iterator> it = metaindex.NewIterator(BytewiseComparator());
  const std::string filter_key = "filter.bloom";
  it->Seek(filter_key);
  ASSERT_TRUE(it->Valid());
  std::string_view val = it->value();
  BlockHandle filter_handle;
  ASSERT_TRUE(filter_handle.DecodeFrom(&val).ok());

  // Corrupt a byte in the filter block.
  FILE* f = std::fopen(path.c_str(), "r+b");
  ASSERT_NE(f, nullptr);
  ASSERT_EQ(std::fseek(f, static_cast<long>(filter_handle.offset()), SEEK_SET), 0);
  int c = std::fgetc(f);
  ASSERT_EQ(std::fseek(f, static_cast<long>(filter_handle.offset()), SEEK_SET), 0);
  std::fputc(c ^ 0x01, f);
  std::fclose(f);

  CommandResult verify_res = RunSstDump({"--verify", path});
  EXPECT_EQ(verify_res.exit_code, 1);
  EXPECT_NE(verify_res.output.find("Corruption"), std::string::npos);
  EXPECT_NE(verify_res.output.find("filter block"), std::string::npos);
}

TEST_F(SstDumpTest, MultipleFiles) {
  std::vector<std::pair<std::string, std::string>> entries1 = {
      {MakeInternalKey("k1", 1, kTypeValue), "v1"},
  };
  std::vector<std::pair<std::string, std::string>> entries2 = {
      {MakeInternalKey("k2", 2, kTypeValue), "v2"},
  };
  std::string path1 = MakeTableFile("f1.sst", 10, entries1);
  std::string path2 = MakeTableFile("f2.sst", 10, entries2);

  CommandResult res = RunSstDump({path1, path2});
  EXPECT_EQ(res.exit_code, 0);
  EXPECT_NE(res.output.find("=== " + path1 + " ==="), std::string::npos);
  EXPECT_NE(res.output.find("=== " + path2 + " ==="), std::string::npos);
  EXPECT_NE(res.output.find("k1 @ 1 : val => v1"), std::string::npos);
  EXPECT_NE(res.output.find("k2 @ 2 : val => v2"), std::string::npos);
}

}  // namespace
}  // namespace strata

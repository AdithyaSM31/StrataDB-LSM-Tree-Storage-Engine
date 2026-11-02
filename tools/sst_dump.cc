#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
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
#include "table/table.h"
#include "util/comparator.h"

namespace strata {
namespace {

std::string EscapeString(std::string_view s) {
  std::string result;
  for (unsigned char c : s) {
    if (c == '\\') {
      result += "\\\\";
    } else if (c == '\n') {
      result += "\\n";
    } else if (c == '\r') {
      result += "\\r";
    } else if (c == '\t') {
      result += "\\t";
    } else if (c < 32 || c >= 127) {
      char buf[5];
      std::snprintf(buf, sizeof(buf), "\\x%02x", c);
      result += buf;
    } else {
      result += static_cast<char>(c);
    }
  }
  return result;
}

Status ReadFooter(RandomAccessFile* file, uint64_t file_size, Footer* footer) {
  if (file_size < Footer::kEncodedLength) {
    return Status::Corruption("file is too short to be an sstable");
  }
  std::string footer_space(Footer::kEncodedLength, '\0');
  std::string_view footer_input;
  Status s = file->Read(file_size - Footer::kEncodedLength, Footer::kEncodedLength, &footer_input,
                        footer_space.data());
  if (!s.ok()) {
    return s;
  }
  if (footer_input.size() != Footer::kEncodedLength) {
    return Status::Corruption("truncated sstable footer");
  }
  return footer->DecodeFrom(footer_input);
}

Status VerifyTable(Env* env, const std::string& filename, uint64_t file_size, const Footer& footer,
                   size_t* out_entries, size_t* out_blocks) {
  *out_entries = 0;
  *out_blocks = 0;

  std::unique_ptr<RandomAccessFile> file;
  Status s = env->NewRandomAccessFile(filename, &file);
  if (!s.ok()) {
    return s;
  }

  // 1. Verify metaindex block.
  std::string metaindex_contents;
  s = ReadBlock(file.get(), file_size, footer.metaindex_handle(), &metaindex_contents);
  if (!s.ok()) {
    return Status::Corruption("metaindex block: " + s.ToString());
  }

  // 2. Check and verify filter blocks if present.
  Block metaindex(std::move(metaindex_contents));
  std::unique_ptr<Iterator> meta_it = metaindex.NewIterator(BytewiseComparator());
  for (meta_it->SeekToFirst(); meta_it->Valid(); meta_it->Next()) {
    if (meta_it->key().rfind("filter.", 0) == 0) {
      std::string_view handle_val = meta_it->value();
      BlockHandle filter_handle;
      s = filter_handle.DecodeFrom(&handle_val);
      if (!s.ok()) {
        return Status::Corruption("filter block handle decode: " + s.ToString());
      }
      std::string filter_contents;
      s = ReadBlock(file.get(), file_size, filter_handle, &filter_contents);
      if (!s.ok()) {
        return Status::Corruption("filter block: " + s.ToString());
      }
    }
  }

  // 3. Verify index block.
  std::string index_contents;
  s = ReadBlock(file.get(), file_size, footer.index_handle(), &index_contents);
  if (!s.ok()) {
    return Status::Corruption("index block: " + s.ToString());
  }

  // 4. Verify each data block.
  InternalKeyComparator icmp;
  Block index_block(std::move(index_contents));
  std::unique_ptr<Iterator> index_it = index_block.NewIterator(&icmp);
  size_t total_blocks = 0;
  size_t total_entries = 0;
  std::string prev_key;
  bool first_key = true;

  for (index_it->SeekToFirst(); index_it->Valid(); index_it->Next()) {
    ++total_blocks;
    std::string_view handle_val = index_it->value();
    BlockHandle data_handle;
    s = data_handle.DecodeFrom(&handle_val);
    if (!s.ok()) {
      return Status::Corruption("data block handle decode in index: " + s.ToString());
    }
    std::string data_contents;
    s = ReadBlock(file.get(), file_size, data_handle, &data_contents);
    if (!s.ok()) {
      return Status::Corruption("data block at offset " + std::to_string(data_handle.offset()) +
                                ": " + s.ToString());
    }
    Block data_block(std::move(data_contents));
    std::unique_ptr<Iterator> data_it = data_block.NewIterator(&icmp);
    std::string block_last_key;
    for (data_it->SeekToFirst(); data_it->Valid(); data_it->Next()) {
      ++total_entries;
      std::string_view key = data_it->key();
      ParsedInternalKey parsed;
      if (!ParseInternalKey(key, &parsed)) {
        return Status::Corruption("malformed internal key at offset " +
                                  std::to_string(data_handle.offset()));
      }
      if (parsed.type != kTypeValue && parsed.type != kTypeDeletion) {
        return Status::Corruption("unknown value type in internal key");
      }
      if (!first_key && icmp.Compare(prev_key, key) >= 0) {
        return Status::Corruption("keys out of order in data block");
      }
      first_key = false;
      prev_key = std::string(key);
      block_last_key = std::string(key);
    }
    if (!data_it->status().ok()) {
      return Status::Corruption("data block iteration error: " + data_it->status().ToString());
    }
    if (!block_last_key.empty() && icmp.Compare(block_last_key, index_it->key()) > 0) {
      return Status::Corruption("last key in block exceeds index key");
    }
  }
  if (!index_it->status().ok()) {
    return Status::Corruption("index block iteration error: " + index_it->status().ToString());
  }

  *out_entries = total_entries;
  *out_blocks = total_blocks;
  return Status::OK();
}

bool ProcessFile(Env* env, const std::string& filename, bool verify, bool show_entries) {
  uint64_t file_size = 0;
  Status s = env->GetFileSize(filename, &file_size);
  if (!s.ok()) {
    std::cerr << filename << ": " << s.ToString() << "\n";
    return false;
  }

  std::unique_ptr<RandomAccessFile> file;
  s = env->NewRandomAccessFile(filename, &file);
  if (!s.ok()) {
    std::cerr << filename << ": " << s.ToString() << "\n";
    return false;
  }

  Footer footer;
  s = ReadFooter(file.get(), file_size, &footer);
  if (!s.ok()) {
    std::cerr << filename << ": " << s.ToString() << "\n";
    return false;
  }

  std::cout << "metaindex handle: offset " << footer.metaindex_handle().offset() << ", size "
            << footer.metaindex_handle().size() << "\n";
  std::cout << "index handle: offset " << footer.index_handle().offset() << ", size "
            << footer.index_handle().size() << "\n";

  Options options;
  InternalKeyComparator icmp;
  std::unique_ptr<Table> table;
  s = Table::Open(options, &icmp, std::move(file), file_size, &table);
  if (!s.ok()) {
    std::cerr << filename << ": " << s.ToString() << "\n";
    return false;
  }

  std::cout << "filter: " << (table->has_filter() ? "present" : "none") << "\n";

  bool ok = true;
  if (show_entries) {
    std::unique_ptr<Iterator> it = table->NewIterator();
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      ParsedInternalKey parsed;
      if (!ParseInternalKey(it->key(), &parsed)) {
        std::cerr << filename << ": Corruption: malformed internal key\n";
        ok = false;
        break;
      }
      std::string type_str;
      if (parsed.type == kTypeValue) {
        type_str = "val";
      } else if (parsed.type == kTypeDeletion) {
        type_str = "del";
      } else {
        type_str = std::to_string(static_cast<uint8_t>(parsed.type));
      }
      std::cout << EscapeString(parsed.user_key) << " @ " << parsed.sequence << " : " << type_str
                << " => " << EscapeString(it->value()) << "\n";
    }
    if (!it->status().ok()) {
      std::cerr << filename << ": Corruption: " << it->status().ToString() << "\n";
      ok = false;
    }
  }

  if (verify) {
    size_t entries = 0;
    size_t blocks = 0;
    s = VerifyTable(env, filename, file_size, footer, &entries, &blocks);
    if (!s.ok()) {
      std::cerr << filename << ": " << s.ToString() << "\n";
      ok = false;
    } else {
      std::cout << "OK: verified " << entries << " entries across " << blocks << " data blocks\n";
    }
  }

  return ok;
}

void PrintUsage(const char* prog) {
  std::cerr << "Usage: " << prog << " [options] <sstable-file...>\n\n"
            << "Options:\n"
            << "  --verify        Scan and verify all blocks, checksums, and keys for corruption\n"
            << "  --show-entries  Print all key-value entries (default if --verify is not set)\n"
            << "  --help, -h      Show this help message\n";
}

}  // namespace
}  // namespace strata

int main(int argc, char** argv) {
  bool verify = false;
  bool show_entries = false;
  bool explicit_show_entries = false;
  std::vector<std::string> filenames;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--verify") {
      verify = true;
    } else if (arg == "--show-entries") {
      show_entries = true;
      explicit_show_entries = true;
    } else if (arg == "--help" || arg == "-h") {
      strata::PrintUsage(argv[0]);
      return 0;
    } else if (!arg.empty() && arg[0] == '-') {
      std::cerr << "Unknown option: " << arg << "\n";
      strata::PrintUsage(argv[0]);
      return 1;
    } else {
      filenames.emplace_back(arg);
    }
  }

  if (filenames.empty()) {
    std::cerr << "No SSTable files specified.\n";
    strata::PrintUsage(argv[0]);
    return 1;
  }

  // If --verify was not requested and --show-entries was not explicitly given,
  // default to showing entries.
  if (!verify && !explicit_show_entries) {
    show_entries = true;
  }

  strata::Env* env = strata::Env::Default();
  bool all_ok = true;
  const bool multiple = filenames.size() > 1;

  for (const auto& filename : filenames) {
    if (multiple) {
      std::cout << "=== " << filename << " ===\n";
    }
    if (!strata::ProcessFile(env, filename, verify, show_entries)) {
      all_ok = false;
    }
  }

  return all_ok ? 0 : 1;
}

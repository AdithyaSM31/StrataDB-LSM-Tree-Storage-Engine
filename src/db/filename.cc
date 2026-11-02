#include "db/filename.h"

#include <charconv>
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

#include "strata/env.h"

namespace strata {

static std::string MakeFileName(const std::string& dbname, uint64_t number, const char* suffix) {
  char buf[100];
  std::snprintf(buf, sizeof(buf), "/%06" PRIu64 ".%s", number, suffix);
  return dbname + buf;
}

std::string LogFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, number, "log");
}

std::string LockFileName(const std::string& dbname) { return dbname + "/LOCK"; }

std::string TableFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, number, "sst");
}

std::string DescriptorFileName(const std::string& dbname, uint64_t number) {
  char buf[100];
  std::snprintf(buf, sizeof(buf), "/MANIFEST-%06" PRIu64, number);
  return dbname + buf;
}

std::string CurrentFileName(const std::string& dbname) { return dbname + "/CURRENT"; }

std::string TempFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, number, "dbtmp");
}

Status SetCurrentFile(Env* env, const std::string& dbname, uint64_t descriptor_number) {
  char manifest_buf[100];
  std::snprintf(manifest_buf, sizeof(manifest_buf), "MANIFEST-%06" PRIu64 "\n", descriptor_number);
  std::string_view contents(manifest_buf);

  std::string tmp = TempFileName(dbname, descriptor_number);
  std::unique_ptr<WritableFile> file;
  Status s = env->NewWritableFile(tmp, &file);
  if (!s.ok()) {
    return s;
  }

  s = file->Append(contents);
  if (s.ok()) {
    s = file->Sync();
  }
  if (s.ok()) {
    s = file->Close();
  }
  if (!s.ok()) {
    (void)env->RemoveFile(tmp);
    return s;
  }

  s = env->RenameFile(tmp, CurrentFileName(dbname));
  if (!s.ok()) {
    (void)env->RemoveFile(tmp);
    return s;
  }

  return env->SyncDir(dbname);
}

bool ParseFileName(std::string_view name, uint64_t* number, FileType* type) {
  if (name == "LOCK") {
    if (number != nullptr) {
      *number = 0;
    }
    if (type != nullptr) {
      *type = FileType::kLockFile;
    }
    return true;
  }
  if (name == "CURRENT") {
    if (number != nullptr) {
      *number = 0;
    }
    if (type != nullptr) {
      *type = FileType::kCurrentFile;
    }
    return true;
  }

  if (name.starts_with("MANIFEST-")) {
    std::string_view num_str = name.substr(std::string_view("MANIFEST-").size());
    if (num_str.empty()) {
      return false;
    }
    uint64_t num = 0;
    auto [ptr, ec] = std::from_chars(num_str.data(), num_str.data() + num_str.size(), num);
    if (ec != std::errc() || ptr != num_str.data() + num_str.size()) {
      return false;
    }
    if (number != nullptr) {
      *number = num;
    }
    if (type != nullptr) {
      *type = FileType::kDescriptorFile;
    }
    return true;
  }

  const auto dot = name.rfind('.');
  if (dot == std::string_view::npos || dot == 0) {
    return false;
  }

  std::string_view num_str = name.substr(0, dot);
  std::string_view ext = name.substr(dot);

  FileType parsed_type;
  if (ext == ".log") {
    parsed_type = FileType::kLogFile;
  } else if (ext == ".sst" || ext == ".ldb") {
    parsed_type = FileType::kTableFile;
  } else if (ext == ".dbtmp") {
    parsed_type = FileType::kTempFile;
  } else {
    return false;
  }

  uint64_t num = 0;
  auto [ptr, ec] = std::from_chars(num_str.data(), num_str.data() + num_str.size(), num);
  if (ec != std::errc() || ptr != num_str.data() + num_str.size()) {
    return false;
  }

  if (number != nullptr) {
    *number = num;
  }
  if (type != nullptr) {
    *type = parsed_type;
  }
  return true;
}

}  // namespace strata

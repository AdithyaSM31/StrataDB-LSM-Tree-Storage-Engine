#include "db/version_set.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "db/dbformat.h"
#include "db/filename.h"
#include "db/version_edit.h"
#include "log/log_reader.h"
#include "log/log_writer.h"
#include "strata/env.h"
#include "strata/options.h"
#include "strata/status.h"
#include "util/check.h"

namespace strata {

namespace {

Status ReadFileToString(Env* env, const std::string& fname, std::string* data) {
  data->clear();
  std::unique_ptr<SequentialFile> file;
  Status s = env->NewSequentialFile(fname, &file);
  if (!s.ok()) {
    return s;
  }
  static constexpr size_t kBufferSize = 8192;
  char space[kBufferSize];
  while (true) {
    std::string_view fragment;
    s = file->Read(kBufferSize, &fragment, space);
    if (!s.ok()) {
      return s;
    }
    data->append(fragment.data(), fragment.size());
    if (fragment.empty()) {
      break;
    }
  }
  return Status::OK();
}

}  // namespace

Version::Version(VersionSet* vset) : vset_(vset) {}

Version::~Version() {
  STRATA_DCHECK(refs_ == 0);
  prev_->next_ = next_;
  next_->prev_ = prev_;

  for (int level = 0; level < kNumLevels; ++level) {
    for (FileMetaData* f : files_[level]) {
      STRATA_DCHECK(f->refs > 0);
      --f->refs;
      if (f->refs == 0) {
        delete f;
      }
    }
  }
}

void Version::Unref() {
  STRATA_DCHECK(refs_ > 0);
  --refs_;
  if (refs_ == 0) {
    delete this;
  }
}

std::string Version::DebugString() const {
  std::ostringstream ss;
  ss << "Version { ";
  for (int level = 0; level < kNumLevels; ++level) {
    ss << "L" << level << ": [";
    for (size_t i = 0; i < files_[level].size(); ++i) {
      if (i > 0) ss << ", ";
      ss << files_[level][i]->number;
    }
    ss << "] ";
  }
  ss << "}";
  return ss.str();
}

class VersionSet::Builder {
 public:
  Builder(VersionSet* vset, Version* base) : vset_(vset), base_(base) { base_->Ref(); }

  ~Builder() {
    for (int level = 0; level < kNumLevels; ++level) {
      for (const auto& [num, f] : added_files_[level]) {
        if (f->refs == 0) {
          delete f;
        }
      }
    }
    base_->Unref();
  }

  void Apply(const VersionEdit* edit) {
    for (const auto& [level, number] : edit->deleted_files()) {
      if (level >= 0 && level < kNumLevels) {
        deleted_files_[level].insert(number);
        auto it = added_files_[level].find(number);
        if (it != added_files_[level].end()) {
          if (it->second->refs == 0) {
            delete it->second;
          }
          added_files_[level].erase(it);
        }
      }
    }

    for (const auto& [level, f_meta] : edit->new_files()) {
      if (level >= 0 && level < kNumLevels) {
        auto* f = new FileMetaData(f_meta);
        f->refs = 0;
        deleted_files_[level].erase(f->number);
        auto it = added_files_[level].find(f->number);
        if (it != added_files_[level].end()) {
          if (it->second->refs == 0) {
            delete it->second;
          }
        }
        added_files_[level][f->number] = f;
      }
    }
  }

  void SaveTo(Version* v) {
    for (int level = 0; level < kNumLevels; ++level) {
      for (FileMetaData* f : base_->files_[level]) {
        if (deleted_files_[level].find(f->number) == deleted_files_[level].end() &&
            added_files_[level].find(f->number) == added_files_[level].end()) {
          v->files_[level].push_back(f);
          f->refs++;
        }
      }

      for (const auto& [num, f] : added_files_[level]) {
        v->files_[level].push_back(f);
        f->refs++;
      }

      if (level == 0) {
        std::sort(
            v->files_[0].begin(), v->files_[0].end(),
            [](const FileMetaData* a, const FileMetaData* b) { return a->number > b->number; });
      } else {
        std::sort(v->files_[level].begin(), v->files_[level].end(),
                  [this](const FileMetaData* a, const FileMetaData* b) {
                    int c = vset_->icmp_->Compare(a->smallest, b->smallest);
                    if (c != 0) {
                      return c < 0;
                    }
                    return a->number < b->number;
                  });
      }
    }
  }

 private:
  VersionSet* vset_;
  Version* base_;
  std::set<uint64_t> deleted_files_[kNumLevels];
  std::map<uint64_t, FileMetaData*> added_files_[kNumLevels];
};

VersionSet::VersionSet(std::string dbname, const Options* options,
                       const InternalKeyComparator* icmp)
    : env_(options->env == nullptr ? Env::Default() : options->env),
      dbname_(std::move(dbname)),
      options_(options),
      icmp_(icmp),
      dummy_versions_(this) {
  dummy_versions_.prev_ = &dummy_versions_;
  dummy_versions_.next_ = &dummy_versions_;
  AppendVersion(new Version(this));
}

VersionSet::~VersionSet() {
  if (current_ != nullptr) {
    current_->Unref();
    current_ = nullptr;
  }
}

void VersionSet::AppendVersion(Version* v) {
  STRATA_DCHECK(v->refs_ == 0);
  STRATA_DCHECK(v != current_);
  if (current_ != nullptr) {
    current_->Unref();
  }
  current_ = v;
  v->Ref();

  v->prev_ = dummy_versions_.prev_;
  v->next_ = &dummy_versions_;
  v->prev_->next_ = v;
  v->next_->prev_ = v;
}

Status VersionSet::LogAndApply(VersionEdit* edit) {
  if (edit->has_log_number()) {
    log_number_ = edit->log_number();
  } else {
    edit->SetLogNumber(log_number_);
  }
  if (edit->has_last_sequence()) {
    SetLastSequence(edit->last_sequence());
  } else {
    edit->SetLastSequence(last_sequence_);
  }
  edit->SetNextFile(next_file_number_);

  auto* v = new Version(this);
  {
    Builder builder(this, current_);
    builder.Apply(edit);
    builder.SaveTo(v);
  }

  Status s;
  if (descriptor_log_ == nullptr) {
    const uint64_t new_manifest_number = NewFileNumber();
    const std::string manifest_fname = DescriptorFileName(dbname_, new_manifest_number);
    std::unique_ptr<WritableFile> new_manifest_file;
    s = env_->NewWritableFile(manifest_fname, &new_manifest_file);
    if (!s.ok()) {
      delete v;
      return s;
    }
    auto new_manifest_log = std::make_unique<log::Writer>(new_manifest_file.get());

    VersionEdit snapshot;
    snapshot.SetComparatorName(icmp_->Name());
    for (int level = 0; level < kNumLevels; ++level) {
      for (const auto* f : current_->files_[level]) {
        snapshot.AddFile(level, f->number, f->file_size, f->smallest, f->largest);
      }
    }
    std::string snapshot_record;
    snapshot.EncodeTo(&snapshot_record);
    s = new_manifest_log->AddRecord(snapshot_record);
    if (s.ok()) {
      s = new_manifest_file->Sync();
    }
    if (s.ok()) {
      s = SetCurrentFile(env_, dbname_, new_manifest_number);
    }
    if (!s.ok()) {
      (void)new_manifest_file->Close();
      (void)env_->RemoveFile(manifest_fname);
      delete v;
      return s;
    }
    manifest_file_number_ = new_manifest_number;
    descriptor_file_ = std::move(new_manifest_file);
    descriptor_log_ = std::move(new_manifest_log);
  }

  edit->SetNextFile(next_file_number_);

  std::string record;
  edit->EncodeTo(&record);
  s = descriptor_log_->AddRecord(record);
  if (s.ok()) {
    s = descriptor_file_->Sync();
  }
  if (!s.ok()) {
    delete v;
    return s;
  }

  AppendVersion(v);
  return Status::OK();
}

Status VersionSet::Recover() {
  const std::string current_fname = CurrentFileName(dbname_);
  std::string current;
  Status s = ReadFileToString(env_, current_fname, &current);
  if (!s.ok()) {
    return s;
  }
  if (current.empty() || current.back() != '\n') {
    return Status::Corruption("CURRENT file does not end with newline");
  }
  std::string_view manifest_name = current;
  manifest_name.remove_suffix(1);
  if (!manifest_name.empty() && manifest_name.back() == '\r') {
    manifest_name.remove_suffix(1);
  }

  const std::string manifest_path = dbname_ + "/" + std::string(manifest_name);
  if (!env_->FileExists(manifest_path)) {
    return Status::Corruption(manifest_path, "MANIFEST file referenced by CURRENT does not exist");
  }

  uint64_t manifest_num = 0;
  FileType type;
  if (ParseFileName(manifest_name, &manifest_num, &type) && type == FileType::kDescriptorFile) {
    manifest_file_number_ = manifest_num;
    MarkFileNumberUsed(manifest_num);
  }

  std::unique_ptr<SequentialFile> file;
  s = env_->NewSequentialFile(manifest_path, &file);
  if (!s.ok()) {
    return s;
  }

  class ManifestReporter : public log::Reader::Reporter {
   public:
    void Corruption(size_t /*bytes*/, const Status& status) override {
      if (status_.ok()) {
        status_ = status;
      }
    }
    const Status& status() const { return status_; }
    bool has_corruption() const { return !status_.ok(); }

   private:
    Status status_;
  };

  ManifestReporter reporter;
  log::Reader reader(file.get(), &reporter, /*checksum=*/true);

  Builder builder(this, current_);
  bool has_log_number = false;
  bool has_next_file_number = false;
  bool has_last_sequence = false;
  uint64_t next_file = 0;
  uint64_t log_num = 0;
  SequenceNumber last_seq = 0;

  std::string_view record;
  std::string scratch;
  while (reader.ReadRecord(&record, &scratch)) {
    if (reporter.has_corruption()) {
      break;
    }
    VersionEdit edit;
    s = edit.DecodeFrom(record);
    if (!s.ok()) {
      return s;
    }
    if (edit.has_comparator()) {
      if (edit.comparator() != icmp_->Name()) {
        return Status::InvalidArgument("comparator mismatch: " + edit.comparator() + " vs " +
                                       icmp_->Name());
      }
    }
    builder.Apply(&edit);

    if (edit.has_log_number()) {
      log_num = edit.log_number();
      has_log_number = true;
    }
    if (edit.has_next_file_number()) {
      next_file = edit.next_file_number();
      has_next_file_number = true;
    }
    if (edit.has_last_sequence()) {
      last_seq = edit.last_sequence();
      has_last_sequence = true;
    }
  }

  if (reporter.has_corruption()) {
    return reporter.status();
  }

  if (!has_next_file_number || !has_log_number || !has_last_sequence) {
    return Status::Corruption("MANIFEST missing required counters");
  }

  MarkFileNumberUsed(log_num);
  next_file_number_ = next_file;
  log_number_ = log_num;
  last_sequence_ = last_seq;

  auto* v = new Version(this);
  builder.SaveTo(v);
  AppendVersion(v);

  return Status::OK();
}

}  // namespace strata

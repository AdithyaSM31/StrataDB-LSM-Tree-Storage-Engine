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
#include "db/table_cache.h"
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

namespace {

// Looks up `key` in one table file. Returns true if this file decides the
// answer, with *s set to: OK (and *value) for a value, NotFound for a
// tombstone, or the error. Returns false if the key is not in this file.
bool SearchFile(const FileMetaData* f, const LookupKey& key, std::string* value, TableCache* cache,
                Status* s) {
  const std::string_view user_key = key.user_key();
  if (user_key.compare(ExtractUserKey(f->smallest)) < 0 ||
      user_key.compare(ExtractUserKey(f->largest)) > 0) {
    return false;  // outside the file's key range: no read needed
  }
  std::shared_ptr<Table> table;
  *s = cache->FindTable(f->number, f->file_size, &table);
  if (!s->ok()) {
    return true;
  }
  bool found = false;
  std::string found_key;
  std::string found_value;
  *s = table->InternalGet(key.internal_key(), &found, &found_key, &found_value);
  if (!s->ok()) {
    return true;
  }
  if (!found || ExtractUserKey(found_key) != user_key) {
    return false;  // the first entry >= the lookup key belongs to another user key
  }
  ParsedInternalKey parsed;
  if (!ParseInternalKey(found_key, &parsed)) {
    *s = Status::Corruption("corrupted internal key in table");
    return true;
  }
  if (parsed.type == kTypeDeletion) {
    // A tombstone decides the answer: older files must not be consulted, or a
    // deleted value would come back.
    *s = Status::NotFound("key deleted");
    return true;
  }
  *value = std::move(found_value);
  *s = Status::OK();
  return true;
}

}  // namespace

Status Version::Get(const LookupKey& key, std::string* value, TableCache* cache) const {
  Status s;
  // L0 files may overlap, so check every one whose range covers the key,
  // newest first (files_[0] is sorted newest first).
  for (const FileMetaData* f : files_[0]) {
    if (SearchFile(f, key, value, cache, &s)) {
      return s;
    }
  }

  // L1 and below never overlap: at most one file per level can hold the key,
  // the first whose largest key is >= the lookup key.
  const InternalKeyComparator icmp;
  for (int level = 1; level < kNumLevels; ++level) {
    const std::vector<FileMetaData*>& files = files_[level];
    auto it = std::lower_bound(files.begin(), files.end(), key.internal_key(),
                               [&icmp](const FileMetaData* f, std::string_view ikey) {
                                 return icmp.Compare(f->largest, ikey) < 0;
                               });
    if (it != files.end() && SearchFile(*it, key, value, cache, &s)) {
      return s;
    }
  }
  return Status::NotFound("key not found");
}

std::string Version::DebugString() const {
  std::ostringstream ss;
  ss << "Version { ";
  for (int level = 0; level < kNumLevels; ++level) {
    ss << "L" << level << ": [";
    for (size_t i = 0; i < files_[level].size(); ++i) {
      if (i > 0) {
        ss << ", ";
      }
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
      icmp_(icmp),
      dummy_versions_() {
  dummy_versions_.prev_ = &dummy_versions_;
  dummy_versions_.next_ = &dummy_versions_;
  AppendVersion(new Version());
}

VersionSet::~VersionSet() {
  if (current_ != nullptr) {
    current_->Unref();
    current_ = nullptr;
  }
  STRATA_DCHECK(dummy_versions_.next_ == &dummy_versions_);
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
    MarkFileNumberUsed(edit->log_number());
  } else {
    edit->SetLogNumber(log_number_);
  }
  if (!edit->has_last_sequence()) {
    edit->SetLastSequence(last_sequence_);
  }
  edit->SetNextFile(next_file_number_);

  auto* v = new Version();
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
      ReuseFileNumber(new_manifest_number);
      delete v;
      return s;
    }
    auto new_manifest_log = std::make_unique<log::Writer>(new_manifest_file.get());

    VersionEdit snapshot;
    snapshot.SetComparatorName(icmp_->Name());
    snapshot.SetLogNumber(log_number_);
    snapshot.SetNextFile(next_file_number_);
    snapshot.SetLastSequence(last_sequence_);
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
    if (!s.ok()) {
      // CURRENT still names the previous MANIFEST, so this one can go.
      (void)new_manifest_file->Close();
      (void)env_->RemoveFile(manifest_fname);
      delete v;
      return s;
    }
    s = SetCurrentFile(env_, dbname_, new_manifest_number);
    if (!s.ok()) {
      // Never delete the new MANIFEST here: SetCurrentFile may already have
      // renamed CURRENT to name it before failing (for example if only the
      // directory sync failed), and deleting it would leave CURRENT pointing at
      // nothing. Either MANIFEST describes the current state (this one holds a
      // full snapshot of it), so whichever CURRENT names is correct. A leftover
      // MANIFEST is harmless; obsolete-file deletion removes it later.
      (void)new_manifest_file->Close();
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
    // The MANIFEST may now end with part of this record. Appending the next
    // edit after it would put a valid record after garbage, which recovery
    // rightly rejects as corruption, so stop using this file. Its partial
    // record is a torn tail that recovery drops silently, and the next
    // LogAndApply starts a fresh MANIFEST with a full snapshot and points
    // CURRENT at it. (DBImpl should also treat this as a sticky error.)
    (void)descriptor_file_->Close();
    descriptor_log_.reset();  // before the file it writes to
    descriptor_file_.reset();
    delete v;
    return s;
  }

  if (edit->has_log_number()) {
    log_number_ = edit->log_number();
  }
  if (edit->has_last_sequence()) {
    SetLastSequence(edit->last_sequence());
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
  if (manifest_name.empty()) {
    return Status::Corruption("CURRENT file is empty");
  }

  uint64_t manifest_num = 0;
  FileType type;
  if (!ParseFileName(manifest_name, &manifest_num, &type) || type != FileType::kDescriptorFile) {
    return Status::Corruption(manifest_name,
                              "CURRENT file does not reference a valid MANIFEST file");
  }

  const std::string manifest_path = dbname_ + "/" + std::string(manifest_name);
  if (!env_->FileExists(manifest_path)) {
    return Status::Corruption(manifest_path, "MANIFEST file referenced by CURRENT does not exist");
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

  manifest_file_number_ = manifest_num;
  next_file_number_ = next_file;
  MarkFileNumberUsed(log_num);
  MarkFileNumberUsed(manifest_file_number_);
  log_number_ = log_num;
  last_sequence_ = last_seq;

  auto* v = new Version();
  builder.SaveTo(v);
  AppendVersion(v);

  return Status::OK();
}

void VersionSet::AddLiveFiles(std::set<uint64_t>* live) const {
  for (const Version* v = dummy_versions_.next_; v != &dummy_versions_; v = v->next_) {
    for (int level = 0; level < kNumLevels; ++level) {
      for (const FileMetaData* f : v->files_[level]) {
        live->insert(f->number);
      }
    }
  }
}

}  // namespace strata

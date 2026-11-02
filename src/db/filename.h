#ifndef STRATA_DB_FILENAME_H_
#define STRATA_DB_FILENAME_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace strata {

enum class FileType { kLogFile, kLockFile, kTableFile, kDescriptorFile, kCurrentFile, kTempFile };

// Return the name of the log file with the specified number
// in the db named by "dbname". The result will be prefixed with "dbname/".
std::string LogFileName(const std::string& dbname, uint64_t number);

// Return the name of the lock file for the db named by "dbname".
// The result will be prefixed with "dbname/".
std::string LockFileName(const std::string& dbname);

// Return the name of the sstable with the specified number
// in the db named by "dbname". The result will be prefixed with "dbname/".
std::string TableFileName(const std::string& dbname, uint64_t number);

// Return the name of the descriptor file with the specified number
// in the db named by "dbname". The result will be prefixed with "dbname/".
std::string DescriptorFileName(const std::string& dbname, uint64_t number);

// Return the name of the CURRENT file.
std::string CurrentFileName(const std::string& dbname);

// Return the name of a temporary file with the specified number.
std::string TempFileName(const std::string& dbname, uint64_t number);

// If filename is a strata file, store its number in *number and its type in
// *type and return true. Otherwise return false.
bool ParseFileName(std::string_view name, uint64_t* number, FileType* type);

}  // namespace strata

#endif  // STRATA_DB_FILENAME_H_

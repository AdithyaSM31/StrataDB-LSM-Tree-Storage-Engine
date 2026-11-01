#ifndef STRATA_INCLUDE_STATUS_H_
#define STRATA_INCLUDE_STATUS_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace strata {

class [[nodiscard]] Status {
 public:
  enum class Code : uint8_t {
    kOk = 0,
    kNotFound = 1,
    kCorruption = 2,
    kNotSupported = 3,
    kInvalidArgument = 4,
    kIOError = 5,
  };

  Status() noexcept = default;

  static Status OK() { return Status(); }
  static Status NotFound(std::string_view msg, std::string_view msg2 = {}) {
    return Status(Code::kNotFound, msg, msg2);
  }
  static Status Corruption(std::string_view msg, std::string_view msg2 = {}) {
    return Status(Code::kCorruption, msg, msg2);
  }
  static Status NotSupported(std::string_view msg, std::string_view msg2 = {}) {
    return Status(Code::kNotSupported, msg, msg2);
  }
  static Status InvalidArgument(std::string_view msg, std::string_view msg2 = {}) {
    return Status(Code::kInvalidArgument, msg, msg2);
  }
  static Status IOError(std::string_view msg, std::string_view msg2 = {}) {
    return Status(Code::kIOError, msg, msg2);
  }

  bool ok() const { return code_ == Code::kOk; }
  bool IsNotFound() const { return code_ == Code::kNotFound; }
  bool IsCorruption() const { return code_ == Code::kCorruption; }
  bool IsNotSupported() const { return code_ == Code::kNotSupported; }
  bool IsInvalidArgument() const { return code_ == Code::kInvalidArgument; }
  bool IsIOError() const { return code_ == Code::kIOError; }

  Code code() const { return code_; }
  std::string_view message() const { return message_; }

  std::string ToString() const;

 private:
  Status(Code code, std::string_view msg, std::string_view msg2 = {});

  Code code_ = Code::kOk;
  std::string message_;
};

using StatusCode = Status::Code;

}  // namespace strata

#endif  // STRATA_INCLUDE_STATUS_H_

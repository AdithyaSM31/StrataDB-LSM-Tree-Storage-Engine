#include "strata/status.h"

namespace strata {

Status::Status(Code code, std::string_view msg, std::string_view msg2) : code_(code) {
  if (msg.empty()) {
    message_ = std::string(msg2);
  } else if (msg2.empty()) {
    message_ = std::string(msg);
  } else {
    message_.reserve(msg.size() + 2 + msg2.size());
    message_.append(msg);
    message_.append(": ");
    message_.append(msg2);
  }
}

std::string Status::ToString() const {
  // Never left as nullptr: building a std::string from nullptr is undefined.
  const char* type = "Unknown code";
  switch (code_) {
    case Code::kOk:
      return "OK";
    case Code::kNotFound:
      type = "NotFound";
      break;
    case Code::kCorruption:
      type = "Corruption";
      break;
    case Code::kNotSupported:
      type = "Not supported";
      break;
    case Code::kInvalidArgument:
      type = "Invalid argument";
      break;
    case Code::kIOError:
      type = "IO error";
      break;
  }

  if (message_.empty()) {
    return type;
  }

  std::string result;
  result.reserve(std::string_view(type).size() + 2 + message_.size());
  result.append(type);
  result.append(": ");
  result.append(message_);
  return result;
}

}  // namespace strata

#ifndef TFDB_STATUS_HPP
#define TFDB_STATUS_HPP

#include <string>

namespace tfdb {

enum class StatusCode {
  ok = 0,
  invalid_argument,
  out_of_range,
  io_error,
  interrupted,
  no_space,
  corrupt,
  unsupported,
  not_found,
  busy,
  overwritten,
  generation_exhausted,
  closed,
  internal_error
};

class Status {
 public:
  Status() : code_(StatusCode::ok) {}
  Status(StatusCode code, const std::string& message)
      : code_(code), message_(message) {}

  static Status Ok() { return Status(); }
  static Status Error(StatusCode code, const std::string& message) {
    return Status(code, message);
  }

  bool ok() const { return code_ == StatusCode::ok; }
  explicit operator bool() const { return ok(); }
  StatusCode code() const { return code_; }
  const std::string& message() const { return message_; }

 private:
  StatusCode code_;
  std::string message_;
};

const char* status_code_name(StatusCode code);

}  // namespace tfdb

#endif  // TFDB_STATUS_HPP

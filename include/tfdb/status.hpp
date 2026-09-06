#ifndef TFDB_STATUS_HPP
#define TFDB_STATUS_HPP

#include <string>

// Every TFDB operation reports expected failures through Status, so silently
// dropping one is a defect. C++17 consumers get that enforced by the compiler;
// a deliberate discard stays spelled (void)call(). C++14 has no portable
// equivalent that a (void) cast can suppress, so the check is opt-in by
// compiling consuming code as C++17 or later.
#ifndef TFDB_NODISCARD
#if defined(__cplusplus) && __cplusplus >= 201703L
#define TFDB_NODISCARD [[nodiscard]]
#else
#define TFDB_NODISCARD
#endif
#endif

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

class TFDB_NODISCARD Status {
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

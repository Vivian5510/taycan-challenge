#pragma once

#include <string>
#include <utility>

class Status {
 public:
  static Status OK() { return Status(Code::kOk, ""); }

  static Status NotFound(std::string msg = "") {
    return Status(Code::kNotFound, std::move(msg));
  }

  static Status IOError(std::string msg = "") {
    return Status(Code::kIOError, std::move(msg));
  }

  bool IsOk() const { return code_ == Code::kOk; }
  bool IsNotFound() const { return code_ == Code::kNotFound; }

  std::string ToString() const {
    const char* name = IsOk() ? "OK" : (IsNotFound() ? "NotFound" : "IOError");
    return message_.empty() ? name : std::string(name) + ": " + message_;
  }

 private:
  enum class Code { kOk, kNotFound, kIOError };

  Status(Code code, std::string msg) : code_(code), message_(std::move(msg)) {}

  Code code_;
  std::string message_;
};

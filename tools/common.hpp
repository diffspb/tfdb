// Copyright (c) 2026 Alexander Safronenko
// SPDX-License-Identifier: BSD-2-Clause

#ifndef TFDB_TOOLS_COMMON_HPP
#define TFDB_TOOLS_COMMON_HPP

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

#include "tfdb/bytes.hpp"
#include "tfdb/status.hpp"

inline bool parse_u64(const char* text, std::uint64_t* output) {
  if (!text || !output || *text == '-') return false;
  errno = 0;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 0);
  if (errno != 0 || !end || *end != '\0') return false;
  *output = static_cast<std::uint64_t>(value);
  return true;
}

inline bool parse_i64(const char* text, std::int64_t* output) {
  if (!text || !output) return false;
  errno = 0;
  char* end = nullptr;
  const long long value = std::strtoll(text, &end, 0);
  if (errno != 0 || !end || *end != '\0') return false;
  *output = static_cast<std::int64_t>(value);
  return true;
}

inline int print_status(const tfdb::Status& status) {
  std::cerr << tfdb::status_code_name(status.code()) << ": " << status.message() << '\n';
  return 2;
}

inline std::string hex_bytes(tfdb::ByteView bytes, std::size_t limit = 64) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  const std::size_t count = bytes.size() < limit ? bytes.size() : limit;
  for (std::size_t i = 0; i != count; ++i)
    out << std::setw(2) << static_cast<unsigned>(bytes.data()[i]);
  if (count != bytes.size()) out << "...";
  return out.str();
}

#endif  // TFDB_TOOLS_COMMON_HPP

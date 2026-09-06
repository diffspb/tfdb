// Copyright (c) 2026 Alexander Safronenko
// SPDX-License-Identifier: BSD-2-Clause

#include "tfdb/status.hpp"

namespace tfdb {

const char* status_code_name(StatusCode code) {
  switch (code) {
    case StatusCode::ok: return "ok";
    case StatusCode::invalid_argument: return "invalid_argument";
    case StatusCode::out_of_range: return "out_of_range";
    case StatusCode::io_error: return "io_error";
    case StatusCode::interrupted: return "interrupted";
    case StatusCode::no_space: return "no_space";
    case StatusCode::corrupt: return "corrupt";
    case StatusCode::unsupported: return "unsupported";
    case StatusCode::not_found: return "not_found";
    case StatusCode::busy: return "busy";
    case StatusCode::overwritten: return "overwritten";
    case StatusCode::generation_exhausted: return "generation_exhausted";
    case StatusCode::closed: return "closed";
    case StatusCode::internal_error: return "internal_error";
  }
  return "unknown";
}

}  // namespace tfdb

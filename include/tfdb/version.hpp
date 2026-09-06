#ifndef TFDB_VERSION_HPP
#define TFDB_VERSION_HPP

// Library source release. This is not the on-media format version: see
// docs/format-v1.md and docs/roadmap.md for the two separate version axes.
// A library release may advance without changing the persistent format.
#define TFDB_VERSION_MAJOR 1
#define TFDB_VERSION_MINOR 0
#define TFDB_VERSION_PATCH 0

#define TFDB_VERSION_STRING "1.0.0"

// Ordered value for conditional compilation, e.g.
//   #if TFDB_VERSION_NUMBER >= 10100
#define TFDB_VERSION_NUMBER \
  (TFDB_VERSION_MAJOR * 10000 + TFDB_VERSION_MINOR * 100 + TFDB_VERSION_PATCH)

namespace tfdb {

// Version of the linked library, which may differ from TFDB_VERSION_STRING
// when headers and libtfdb.a come from different releases.
const char* library_version();

}  // namespace tfdb

#endif  // TFDB_VERSION_HPP

// Copyright (c) 2026 Alexander Safronenko
// SPDX-License-Identifier: BSD-2-Clause

#ifndef TFDB_RECORD_PROFILE_HPP
#define TFDB_RECORD_PROFILE_HPP

#include <cstdint>
#include <functional>
#include <vector>

#include "tfdb/bytes.hpp"
#include "tfdb/ring_store.hpp"
#include "tfdb/status.hpp"

namespace tfdb {

struct RecordView {
  // Both views are valid only during the RecordVisitor/decode callback.
  ByteView encoded;
  ByteView payload;
  std::int64_t index_time_ns = 0;
  std::uint64_t selector = 0;
  std::uint16_t flags = 0;
};

using RecordDecodeVisitor = std::function<bool(const RecordView&)>;

class RecordProfile {
 public:
  virtual ~RecordProfile() {}
  virtual std::uint64_t id() const = 0;
  virtual std::uint16_t version() const = 0;
  virtual Status decode_block(ByteView block,
                              const RecordDecodeVisitor& visitor) const = 0;
};

struct RecordQuery {
  TimeRange time;
  std::vector<std::uint64_t> selectors;  // Empty means all selectors.
  QueryOptions block_options;
};

struct RecordEvent {
  BlockEventKind kind = BlockEventKind::data;
  BlockMetadata block;
  RecordView record;
  Status detail;
};

using RecordVisitor = std::function<bool(const RecordEvent&)>;

Status query_records(const RingStore& store, const RecordQuery& query,
                     const RecordProfile& profile,
                     const RecordVisitor& visitor);

// Optional profile for projects whose native records do not already contain
// the application-provided index timestamp and selector. Its 32-byte envelope
// is deliberately outside the block format, so self-framing telemetry pays no
// double-framing cost.
constexpr std::uint64_t kFramedRecordV1ProfileId = 0x5446444252465631ull;

class FramedRecordV1 final : public RecordProfile {
 public:
  std::uint64_t id() const override { return kFramedRecordV1ProfileId; }
  std::uint16_t version() const override { return 1; }
  Status decode_block(ByteView block,
                      const RecordDecodeVisitor& visitor) const override;

  // Payload may view the current contents of *output; encoding is alias-safe.
  static Status encode(std::int64_t index_time_ns, std::uint64_t selector,
                       std::uint16_t flags, ByteView payload,
                       std::vector<std::uint8_t>* output);
};

Status append_framed_record(RingStore& store, std::int64_t index_time_ns,
                            std::uint64_t selector, std::uint16_t flags,
                            ByteView payload);

}  // namespace tfdb

#endif  // TFDB_RECORD_PROFILE_HPP

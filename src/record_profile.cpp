// Copyright (c) 2026 Alexander Safronenko
// SPDX-License-Identifier: BSD-2-Clause

#include "tfdb/record_profile.hpp"

#include <algorithm>
#include <limits>
#include <set>

#include "internal_format.hpp"

namespace tfdb {
namespace {

void put16(std::uint8_t* out, std::uint16_t value) {
  out[0] = static_cast<std::uint8_t>(value);
  out[1] = static_cast<std::uint8_t>(value >> 8);
}
void put32(std::uint8_t* out, std::uint32_t value) {
  for (unsigned i = 0; i != 4; ++i) out[i] = static_cast<std::uint8_t>(value >> (i * 8));
}
void put64(std::uint8_t* out, std::uint64_t value) {
  for (unsigned i = 0; i != 8; ++i) out[i] = static_cast<std::uint8_t>(value >> (i * 8));
}
std::uint16_t get16(const std::uint8_t* in) {
  return static_cast<std::uint16_t>(in[0]) | static_cast<std::uint16_t>(in[1]) << 8;
}
std::uint32_t get32(const std::uint8_t* in) {
  std::uint32_t value = 0;
  for (unsigned i = 0; i != 4; ++i) value |= static_cast<std::uint32_t>(in[i]) << (i * 8);
  return value;
}
std::uint64_t get64(const std::uint8_t* in) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i != 8; ++i) value |= static_cast<std::uint64_t>(in[i]) << (i * 8);
  return value;
}

}  // namespace

Status FramedRecordV1::encode(std::int64_t time, std::uint64_t selector,
                              std::uint16_t flags, ByteView payload,
                              std::vector<std::uint8_t>* output) {
  if (!output) return Status::Error(StatusCode::invalid_argument, "null framed record output");
  if (!payload.valid())
    return Status::Error(StatusCode::invalid_argument,
                         "null non-empty framed payload");
  constexpr std::size_t header_size = 32;
  if (payload.size() > UINT32_MAX - header_size)
    return Status::Error(StatusCode::out_of_range, "framed payload too large");
  std::vector<std::uint8_t> encoded(header_size + payload.size(), 0);
  put32(encoded.data(), static_cast<std::uint32_t>(encoded.size()));
  put16(encoded.data() + 4, header_size);
  put16(encoded.data() + 6, flags);
  put64(encoded.data() + 8, static_cast<std::uint64_t>(time));
  put64(encoded.data() + 16, selector);
  put32(encoded.data() + 24, static_cast<std::uint32_t>(payload.size()));
  if (!payload.empty())
    std::copy(payload.data(), payload.data() + payload.size(),
              encoded.begin() + header_size);
  // The CRC covers framing fields and payload. The CRC field itself is zero
  // while calculating, so corrupted selectors/timestamps cannot be accepted.
  put32(encoded.data() + 28, internal::crc32c(ByteView(encoded)));
  output->swap(encoded);
  return Status::Ok();
}

Status FramedRecordV1::decode_block(ByteView block,
                                    const RecordDecodeVisitor& visitor) const {
  if (!visitor) return Status::Error(StatusCode::invalid_argument, "empty record visitor");
  if (!block.valid())
    return Status::Error(StatusCode::invalid_argument,
                         "null non-empty framed block");
  std::size_t offset = 0;
  while (offset < block.size()) {
    if (block.size() - offset < 32)
      return Status::Error(StatusCode::corrupt, "truncated FramedRecordV1 header");
    const std::uint8_t* data = block.data() + offset;
    const std::uint32_t frame_size = get32(data);
    const std::uint16_t header_size = get16(data + 4);
    const std::uint32_t payload_size = get32(data + 24);
    if (header_size != 32 || frame_size < header_size ||
        frame_size != header_size + payload_size || frame_size > block.size() - offset)
      return Status::Error(StatusCode::corrupt, "invalid FramedRecordV1 bounds");
    ByteView payload(data + header_size, payload_size);
    const std::uint32_t expected_crc = get32(data + 28);
    if (internal::crc32c_with_zeroed_field(ByteView(data, frame_size), 28) !=
        expected_crc)
      return Status::Error(StatusCode::corrupt, "FramedRecordV1 frame CRC mismatch");
    RecordView record;
    record.encoded = ByteView(data, frame_size);
    record.payload = payload;
    const std::uint64_t encoded_time = get64(data + 8);
    if ((encoded_time & (std::uint64_t{1} << 63)) == 0) {
      record.index_time_ns = static_cast<std::int64_t>(encoded_time);
    } else {
      const std::uint64_t magnitude = (~encoded_time) + 1u;
      record.index_time_ns = magnitude == (std::uint64_t{1} << 63)
          ? std::numeric_limits<std::int64_t>::min()
          : -static_cast<std::int64_t>(magnitude);
    }
    record.selector = get64(data + 16);
    record.flags = get16(data + 6);
    if (!visitor(record)) return Status::Ok();
    offset += frame_size;
  }
  return Status::Ok();
}

Status append_framed_record(RingStore& store, std::int64_t time,
                            std::uint64_t selector, std::uint16_t flags,
                            ByteView payload) {
  PartitionInfo active;
  Status status = store.active_partition_info(&active);
  if (!status.ok()) return status;
  if (active.options.record_format_id != kFramedRecordV1ProfileId ||
      active.options.record_format_version != 1)
    return Status::Error(StatusCode::invalid_argument,
                         "active partition is not FramedRecordV1");
  std::vector<std::uint8_t> encoded;
  status = FramedRecordV1::encode(time, selector, flags, payload, &encoded);
  if (!status.ok()) return status;
  std::uint32_t storage_flags = 0;
  if (flags & kRecordFlagUnsynchronizedTime)
    storage_flags |= kRecordFlagUnsynchronizedTime;
  AppendContract expected;
  expected.record_format_id = kFramedRecordV1ProfileId;
  expected.record_format_version = 1;
  expected.time_domain_id = active.options.time_domain_id;
  return store.append_checked(ByteView(encoded), time, expected, storage_flags);
}

Status query_records(const RingStore& store, const RecordQuery& query,
                     const RecordProfile& profile,
                     const RecordVisitor& visitor) {
  if (!visitor) return Status::Error(StatusCode::invalid_argument, "empty record visitor");
  if (query.time.end_ns <= query.time.begin_ns)
    return Status::Error(StatusCode::invalid_argument, "record query needs non-empty [begin,end)");
  const std::set<std::uint64_t> selectors(query.selectors.begin(), query.selectors.end());
  bool keep_going = true;
  Status decode_status;
  Status status = store.query_blocks(query.time, query.block_options,
      [&](const BlockEvent& block_event) {
        if (block_event.kind != BlockEventKind::data) {
          RecordEvent event;
          event.kind = block_event.kind;
          event.block = block_event.metadata;
          event.detail = block_event.detail;
          keep_going = visitor(event);
          return keep_going;
        }
        if (block_event.metadata.record_format_id != profile.id() ||
            block_event.metadata.record_format_version != profile.version()) {
          decode_status = Status::Error(StatusCode::unsupported,
                                        "record profile does not match partition");
          return false;
        }
        std::uint32_t decoded_records = 0;
        decode_status = profile.decode_block(block_event.data,
            [&](const RecordView& record) {
              ++decoded_records;
              if (record.index_time_ns < query.time.begin_ns ||
                  record.index_time_ns >= query.time.end_ns) return true;
              if (!selectors.empty() && selectors.count(record.selector) == 0) return true;
              RecordEvent event;
              event.kind = BlockEventKind::data;
              event.block = block_event.metadata;
              event.record = record;
              keep_going = visitor(event);
              return keep_going;
            });
        if (decode_status.ok() && keep_going &&
            decoded_records != block_event.metadata.record_count)
          decode_status = Status::Error(StatusCode::corrupt,
                                        "block record count does not match profile decode");
        return keep_going && decode_status.ok();
      });
  if (!status.ok()) return status;
  return decode_status;
}

}  // namespace tfdb

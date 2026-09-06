// Copyright (c) 2026 Alexander Safronenko
// SPDX-License-Identifier: BSD-2-Clause

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "tfdb/async_writer.hpp"
#include "tfdb/record_profile.hpp"
#include "tfdb/ring_store.hpp"
#include "tfdb/status.hpp"
#include "tfdb/storage.hpp"

namespace {

void require(const tfdb::Status& status, const char* operation) {
  if (status.ok()) return;
  throw std::runtime_error(
      std::string(operation) + ": " + tfdb::status_code_name(status.code()) +
      ": " + status.message());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: tfdb_async_writer NEW_STORE_PATH\n";
    return 2;
  }

  try {
    constexpr std::uint64_t kMiB = 1024u * 1024u;
    constexpr std::int64_t kBaseTimeNs = 1700000000000000000ll;
    constexpr std::uint64_t kSelector = 0x020001u;

    std::shared_ptr<tfdb::FileStorage> storage;
    require(tfdb::FileStorage::create(argv[1], 3u * kMiB, false, &storage),
            "create storage");

    tfdb::VolumeOptions volume;
    volume.partition_size = 1u * kMiB;
    volume.index_region_size = 64u * 1024u;
    volume.max_block_payload = 8u * 1024u;
    volume.persistence_quantum = 4096u;
    require(tfdb::RingStore::format(*storage, volume), "format storage");

    tfdb::OpenOptions open;
    open.writable = true;
    open.next_partition.record_format_id = tfdb::kFramedRecordV1ProfileId;
    open.next_partition.record_format_version = 1;
    open.next_partition.time_domain_id = 1;
    std::unique_ptr<tfdb::RingStore> store;
    require(tfdb::RingStore::open(storage, open, &store), "open store");

    tfdb::AsyncWriterMetrics metrics;
    {
      tfdb::AsyncWriterOptions writer_options;
      writer_options.queue_capacity_records = 16;
      writer_options.queue_capacity_bytes = 64u * 1024u;
      writer_options.checkpoint_interval = std::chrono::milliseconds(250);
      tfdb::AsyncWriter writer(*store, writer_options);
      require(writer.start(), "start async writer");

      tfdb::AppendContract contract;
      contract.record_format_id = tfdb::kFramedRecordV1ProfileId;
      contract.record_format_version = 1;
      contract.time_domain_id = 1;

      for (std::uint64_t ordinal = 0; ordinal != 10; ++ordinal) {
        const std::string payload = "sample=" + std::to_string(ordinal);
        std::vector<std::uint8_t> encoded;
        require(tfdb::FramedRecordV1::encode(
                    kBaseTimeNs + static_cast<std::int64_t>(ordinal),
                    kSelector, 0,
                    tfdb::ByteView(payload.data(), payload.size()), &encoded),
                "encode framed record");
        // submit_checked() copies encoded before returning; this local buffer
        // may be reused immediately. StatusCode::busy means the record was not
        // accepted. This single-producer example drains once and retries once;
        // production code needs a bounded retry/drop/backpressure policy.
        tfdb::Status submitted = writer.submit_checked(
            tfdb::ByteView(encoded),
            kBaseTimeNs + static_cast<std::int64_t>(ordinal), contract);
        if (submitted.code() == tfdb::StatusCode::busy) {
          require(writer.checkpoint(), "checkpoint after backpressure");
          submitted = writer.submit_checked(
              tfdb::ByteView(encoded),
              kBaseTimeNs + static_cast<std::int64_t>(ordinal), contract);
        }
        require(submitted, "submit record");
      }

      // Wait until every record accepted before this call is durable.
      require(writer.checkpoint(), "async checkpoint");
      require(writer.stop(), "stop async writer");
      metrics = writer.metrics();
      if (metrics.durable_sequence != 10 || metrics.background_errors != 0) {
        std::cerr << "unexpected writer metrics\n";
        return 3;
      }
    }

    require(store->close(), "close writable store");
    store.reset();
    storage.reset();
    require(tfdb::FileStorage::open_existing(argv[1], false, &storage),
            "reopen storage read-only");
    tfdb::OpenOptions read_only;
    require(tfdb::RingStore::open(storage, read_only, &store),
            "reopen store read-only");

    tfdb::RecordQuery query;
    query.time.begin_ns = kBaseTimeNs;
    query.time.end_ns = kBaseTimeNs + 10;
    query.time.time_domain_id = 1;
    query.selectors.push_back(kSelector);
    tfdb::FramedRecordV1 profile;
    std::size_t matches = 0;
    bool saw_gap = false;
    require(tfdb::query_records(
                *store, query, profile,
                [&](const tfdb::RecordEvent& event) {
                  if (event.kind != tfdb::BlockEventKind::data) {
                    saw_gap = true;
                    std::cerr << "query gap: " << event.detail.message()
                              << '\n';
                    return true;
                  }
                  ++matches;
                  return true;
                }),
            "query records");
    if (saw_gap || matches != 10) {
      std::cerr << "unexpected query result: matches=" << matches
                << " gap=" << saw_gap << '\n';
      return 4;
    }

    require(store->close(), "close read-only store");
    std::cout << "durable records=" << metrics.durable_sequence
              << " checkpoints=" << metrics.checkpoint_calls << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

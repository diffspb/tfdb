#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

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
    std::cerr << "usage: tfdb_quickstart NEW_STORE_PATH\n";
    return 2;
  }

  try {
    constexpr std::uint64_t kMiB = 1024u * 1024u;
    constexpr std::int64_t kBaseTimeNs = 1700000000000000000ll;
    constexpr std::uint64_t kTemperatureSelector = 0x010001u;
    constexpr std::uint64_t kPressureSelector = 0x010002u;

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

    const std::string temperature_1 = "temperature=21.5";
    const std::string pressure = "pressure=101.3";
    const std::string temperature_2 = "temperature=21.7";
    require(tfdb::append_framed_record(
                *store, kBaseTimeNs, kTemperatureSelector, 0,
                tfdb::ByteView(temperature_1.data(), temperature_1.size())),
            "append first temperature");
    require(tfdb::append_framed_record(
                *store, kBaseTimeNs + 1000, kPressureSelector, 0,
                tfdb::ByteView(pressure.data(), pressure.size())),
            "append pressure");
    require(tfdb::append_framed_record(
                *store, kBaseTimeNs + 2000, kTemperatureSelector, 0,
                tfdb::ByteView(temperature_2.data(), temperature_2.size())),
            "append second temperature");

    // This is the explicit durability boundary for everything accepted above.
    require(store->checkpoint(), "checkpoint");
    require(store->close(), "close writable store");

    // Reopen through a fresh read-only backend so this example checks the
    // persisted/recovered view rather than the writer's in-memory catalog.
    store.reset();
    storage.reset();
    require(tfdb::FileStorage::open_existing(argv[1], false, &storage),
            "reopen storage read-only");
    tfdb::OpenOptions read_only;
    require(tfdb::RingStore::open(storage, read_only, &store),
            "reopen store read-only");

    tfdb::RecordQuery query;
    query.time.begin_ns = kBaseTimeNs;
    query.time.end_ns = kBaseTimeNs + 3000;
    query.time.time_domain_id = 1;
    query.selectors.push_back(kTemperatureSelector);

    tfdb::FramedRecordV1 profile;
    std::size_t matches = 0;
    bool saw_gap = false;
    require(tfdb::query_records(
                *store, query, profile,
                [&](const tfdb::RecordEvent& event) {
                  if (event.kind != tfdb::BlockEventKind::data) {
                    saw_gap = true;
                    std::cerr << "query gap: " << event.detail.message() << '\n';
                    return true;
                  }
                  ++matches;
                  const tfdb::ByteView payload = event.record.payload;
                  std::cout.write(
                      reinterpret_cast<const char*>(payload.data()),
                      static_cast<std::streamsize>(payload.size()));
                  std::cout << '\n';
                  return true;
                }),
            "query records");

    if (saw_gap || matches != 2) {
      std::cerr << "unexpected query result: matches=" << matches
                << " gap=" << saw_gap << '\n';
      return 3;
    }

    require(store->close(), "close read-only store");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

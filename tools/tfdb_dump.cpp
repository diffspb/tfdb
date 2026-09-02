#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "common.hpp"
#include "tfdb/record_profile.hpp"
#include "tfdb/ring_store.hpp"
#include "tfdb/storage.hpp"

namespace {

void usage() {
  std::cerr << "usage: tfdb_dump PATH [--from NS --to NS] [--selector ID] "
               "[--time-domain ID] [--framed-v1 | --raw-blocks]\n";
}

void write_u32(std::uint32_t value) {
  const char bytes[4] = {static_cast<char>(value), static_cast<char>(value >> 8),
                         static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
  std::cout.write(bytes, 4);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { usage(); return 1; }
  const std::string path = argv[1];
  bool framed = false, raw = false, have_range = false;
  std::int64_t from = std::numeric_limits<std::int64_t>::min();
  std::int64_t to = std::numeric_limits<std::int64_t>::max();
  std::vector<std::uint64_t> selectors;
  std::uint64_t time_domain_id = 1;
  for (int i = 2; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--framed-v1") framed = true;
    else if (argument == "--raw-blocks") raw = true;
    else if (argument == "--from" && i + 1 < argc) {
      if (!parse_i64(argv[++i], &from)) { usage(); return 1; }
      have_range = true;
    } else if (argument == "--to" && i + 1 < argc) {
      if (!parse_i64(argv[++i], &to)) { usage(); return 1; }
      have_range = true;
    } else if (argument == "--selector" && i + 1 < argc) {
      std::uint64_t value = 0;
      if (!parse_u64(argv[++i], &value)) { usage(); return 1; }
      selectors.push_back(value);
    } else if (argument == "--time-domain" && i + 1 < argc) {
      if (!parse_u64(argv[++i], &time_domain_id) || time_domain_id == 0) {
        usage(); return 1;
      }
    } else { usage(); return 1; }
  }
  if (framed && raw) { usage(); return 1; }
  std::shared_ptr<tfdb::FileStorage> storage;
  tfdb::Status status = tfdb::FileStorage::open_existing(path, false, &storage);
  if (!status.ok()) return print_status(status);
  std::unique_ptr<tfdb::RingStore> store;
  tfdb::OpenOptions open;
  status = tfdb::RingStore::open(storage, open, &store);
  if (!status.ok()) return print_status(status);
  bool saw_gap = false;
  if (framed) {
    if (to <= from) { usage(); return 1; }
    tfdb::RecordQuery query;
    query.time = {from, to};
    query.time.time_domain_id = time_domain_id;
    query.selectors = selectors;
    tfdb::FramedRecordV1 profile;
    status = tfdb::query_records(*store, query, profile,
        [&](const tfdb::RecordEvent& event) {
          if (event.kind != tfdb::BlockEventKind::data) {
            saw_gap = true;
            std::cerr << "gap " << event.detail.message() << '\n';
            return true;
          }
          std::cout << "generation=" << event.block.partition_generation
                    << " block=" << event.block.block_sequence
                    << " time=" << event.record.index_time_ns
                    << " selector=" << event.record.selector
                    << " flags=0x" << std::hex << event.record.flags << std::dec
                    << " payload=" << hex_bytes(event.record.payload) << '\n';
          return true;
        });
  } else {
    tfdb::QueryOptions options;
    auto visitor = [&](const tfdb::BlockEvent& event) {
      if (event.kind != tfdb::BlockEventKind::data) {
        saw_gap = true;
        std::cerr << "gap " << event.detail.message() << '\n';
        return true;
      }
      if (raw) {
        write_u32(static_cast<std::uint32_t>(event.data.size()));
        std::cout.write(reinterpret_cast<const char*>(event.data.data()), event.data.size());
      } else {
        std::cout << "generation=" << event.metadata.partition_generation
                  << " block=" << event.metadata.block_sequence
                  << " records=" << event.metadata.record_count
                  << " min_time=" << event.metadata.min_time_ns
                  << " max_time=" << event.metadata.max_time_ns
                  << " raw_bytes=" << event.data.size()
                  << " data=" << hex_bytes(event.data) << '\n';
      }
      return true;
    };
    status = have_range
        ? store->query_blocks({from, to, time_domain_id}, options, visitor)
                        : store->scan_blocks(options, visitor);
  }
  if (!status.ok()) return print_status(status);
  if (!std::cout.good()) return 4;
  return saw_gap ? 3 : 0;
}

#include <cstdint>
#include <iostream>
#include <memory>

#include "common.hpp"
#include "tfdb/ring_store.hpp"
#include "tfdb/storage.hpp"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: tfdb_verify PATH\n";
    return 1;
  }
  std::shared_ptr<tfdb::FileStorage> storage;
  tfdb::Status status = tfdb::FileStorage::open_existing(argv[1], false, &storage);
  if (!status.ok()) return print_status(status);
  std::unique_ptr<tfdb::RingStore> store;
  tfdb::OpenOptions open;
  status = tfdb::RingStore::open(storage, open, &store);
  if (!status.ok()) return print_status(status);
  std::uint64_t blocks = 0, raw_bytes = 0, gaps = 0;
  tfdb::QueryOptions options;
  options.continue_on_gap = true;
  status = store->scan_blocks(options, [&](const tfdb::BlockEvent& event) {
    if (event.kind == tfdb::BlockEventKind::data) {
      ++blocks;
      raw_bytes += event.data.size();
    } else {
      ++gaps;
      std::cerr << "gap generation=" << event.metadata.partition_generation
                << " block=" << event.metadata.block_sequence
                << " reason=" << event.detail.message() << '\n';
    }
    return true;
  });
  if (!status.ok() && gaps == 0) return print_status(status);
  std::cout << "verified blocks=" << blocks << " raw_bytes=" << raw_bytes
            << " gaps=" << gaps << '\n';
  return gaps == 0 ? 0 : 3;
}

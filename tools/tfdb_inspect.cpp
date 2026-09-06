// Copyright (c) 2026 Alexander Safronenko
// SPDX-License-Identifier: BSD-2-Clause

#include <iomanip>
#include <iostream>
#include <memory>
#include <string>

#include "common.hpp"
#include "tfdb/ring_store.hpp"
#include "tfdb/storage.hpp"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: tfdb_inspect PATH\n";
    return 1;
  }
  std::shared_ptr<tfdb::FileStorage> storage;
  tfdb::Status status = tfdb::FileStorage::open_existing(argv[1], false, &storage);
  if (!status.ok()) return print_status(status);
  std::unique_ptr<tfdb::RingStore> store;
  tfdb::OpenOptions open;
  status = tfdb::RingStore::open(storage, open, &store);
  if (!status.ok()) return print_status(status);
  tfdb::VolumeInfo info;
  status = store->inspect(&info);
  if (!status.ok()) return print_status(status);
  std::cout << "volume id=" << std::hex << std::setw(16) << std::setfill('0')
            << info.volume_id_high << std::setw(16) << info.volume_id_low << std::dec
            << " bytes=" << info.volume_size
            << " partitions=" << info.partition_count
            << " partition_bytes=" << info.partition_size
            << " block_payload_bytes=" << info.max_block_payload
            << " quantum=" << info.persistence_quantum
            << " index_bytes=" << info.index_region_size << '\n';
  for (const tfdb::PartitionInfo& partition : info.partitions) {
    std::cout << "partition slot=" << partition.slot
              << " generation=" << partition.generation
              << " state=" << (partition.sealed ? "sealed" : "active")
              << " blocks=" << partition.block_count
              << " data_span_bytes=" << partition.data_bytes
              << " min_time=" << partition.min_time_ns
              << " max_time=" << partition.max_time_ns
              << " flags=0x" << std::hex << partition.flags << std::dec
              << " compression=" << static_cast<unsigned>(partition.options.compression)
              << " record_profile=" << partition.options.record_format_id
              << ':' << partition.options.record_format_version
              << " time_domain=" << partition.options.time_domain_id << '\n';
  }
  return 0;
}

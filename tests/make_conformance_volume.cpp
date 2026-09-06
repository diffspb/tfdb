// Copyright (c) 2026 Alexander Safronenko
// SPDX-License-Identifier: BSD-2-Clause

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "tfdb/record_profile.hpp"
#include "tfdb/ring_store.hpp"
#include "tfdb/storage.hpp"

namespace {

bool check(const tfdb::Status& status) {
  if (status.ok()) return true;
  std::cerr << tfdb::status_code_name(status.code()) << ": "
            << status.message() << '\n';
  return false;
}

std::vector<std::uint8_t> payload(std::uint8_t value, std::size_t size) {
  return std::vector<std::uint8_t>(size, value);
}

bool append(tfdb::RingStore& store, std::int64_t time, std::uint64_t selector,
            std::uint16_t flags, const std::vector<std::uint8_t>& bytes) {
  return check(tfdb::append_framed_record(
      store, time, selector, flags, tfdb::ByteView(bytes)));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: tfdb_make_conformance_volume PATH\n";
    return 2;
  }
  constexpr std::uint64_t partition_size = 65536;
  constexpr std::uint64_t volume_size = 8192 + 2 * partition_size;
  std::shared_ptr<tfdb::FileStorage> storage;
  if (!check(tfdb::FileStorage::create(argv[1], volume_size, true, &storage)))
    return 1;

  tfdb::VolumeOptions volume;
  volume.partition_size = partition_size;
  volume.index_region_size = 4096;
  volume.max_block_payload = 256;
  volume.persistence_quantum = 512;
  volume.volume_id_high = 0x0102030405060708ull;
  volume.volume_id_low = 0x1112131415161718ull;
  volume.allow_explicit_volume_id_for_testing = true;
  volume.created_time_ns = -2;
  if (!check(tfdb::RingStore::format(*storage, volume))) return 1;

  tfdb::PartitionOptions first;
  first.compression = tfdb::CompressionId::packbits;
  first.compression_version = 1;
  first.record_format_id = tfdb::kFramedRecordV1ProfileId;
  first.record_format_version = 1;
  first.time_domain_id = 42;
  first.allowed_backward_skew_ns = 50;
  first.allowed_forward_step_ns = 500;

  tfdb::OpenOptions open;
  open.writable = true;
  open.next_partition = first;
  open.writer_id_high = 0x2122232425262728ull;
  open.writer_id_low = 0x3132333435363738ull;
  open.allow_explicit_writer_id_for_testing = true;
  std::unique_ptr<tfdb::RingStore> store;
  if (!check(tfdb::RingStore::open(storage, open, &store))) return 1;

  if (!append(*store, -100, 10, tfdb::kRecordFlagUnsynchronizedTime,
              payload('A', 120)) ||
      !append(*store, 0, 11, 0, payload('B', 17)) ||
      !append(*store, 1000, 10, 0, payload('C', 120)) ||
      !append(*store, 900, 12, 0, payload('D', 9)) ||
      !append(*store, 1600, 13, 0, payload('E', 120)) ||
      !check(store->checkpoint())) return 1;

  tfdb::PartitionOptions second = first;
  second.compression = tfdb::CompressionId::none;
  second.time_domain_id = 43;
  second.allowed_backward_skew_ns = -1;
  second.allowed_forward_step_ns = -1;
  if (!check(store->rotate(second)) ||
      !append(*store, 2000, 20, 0, payload('x', 3)) ||
      !append(*store, 1500, 21, 0, payload('y', 64)) ||
      !append(*store, 2500, 20, 0, payload('z', 5)) ||
      !check(store->close())) return 1;
  return 0;
}

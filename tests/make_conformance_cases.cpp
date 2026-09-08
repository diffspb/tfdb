// Copyright (c) 2026 Alexander Safronenko
// SPDX-License-Identifier: BSD-2-Clause

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "tfdb/memory_storage.hpp"
#include "tfdb/ring_store.hpp"

namespace {

constexpr std::uint64_t kPartitionSize = 65536;
constexpr std::uint64_t kVolumePrefixSize = 8192;

bool check(const tfdb::Status& status) {
  if (status.ok()) return true;
  std::cerr << tfdb::status_code_name(status.code()) << ": "
            << status.message() << '\n';
  return false;
}

std::string joined(const std::string& directory, const char* name) {
  if (!directory.empty() && directory.back() == '/') return directory + name;
  return directory + "/" + name;
}

bool read_image(const std::string& path, std::vector<std::uint8_t>* bytes) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    std::cerr << "cannot open input image: " << path << '\n';
    return false;
  }
  const std::streamoff size = input.tellg();
  if (size < 0 || static_cast<std::uint64_t>(size) >
                      std::numeric_limits<std::size_t>::max()) {
    std::cerr << "invalid input image size: " << path << '\n';
    return false;
  }
  bytes->assign(static_cast<std::size_t>(size), 0);
  input.seekg(0);
  input.read(reinterpret_cast<char*>(bytes->data()),
             static_cast<std::streamsize>(size));
  if (!input) {
    std::cerr << "cannot read input image: " << path << '\n';
    return false;
  }
  return true;
}

bool write_image(const std::string& path,
                 const std::vector<std::uint8_t>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    std::cerr << "cannot write output image: " << path << '\n';
    return false;
  }
  return true;
}

void put16(std::vector<std::uint8_t>* bytes, std::size_t offset,
           std::uint16_t value) {
  for (unsigned i = 0; i != 2; ++i)
    (*bytes)[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

void put32(std::vector<std::uint8_t>* bytes, std::size_t offset,
           std::uint32_t value) {
  for (unsigned i = 0; i != 4; ++i)
    (*bytes)[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

void put64(std::vector<std::uint8_t>* bytes, std::size_t offset,
           std::uint64_t value) {
  for (unsigned i = 0; i != 8; ++i)
    (*bytes)[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

std::uint32_t crc32c(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xffffffffu;
  for (std::size_t i = 0; i != size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit != 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1u) != 0 ? 0x82f63b78u : 0u);
  }
  return ~crc;
}

void repair_crc(std::vector<std::uint8_t>* image, std::size_t begin,
                std::size_t size, std::size_t relative_crc_offset) {
  put32(image, begin + relative_crc_offset, 0);
  put32(image, begin + relative_crc_offset,
        crc32c(image->data() + begin, size));
}

bool write_unknown_feature_case(const std::vector<std::uint8_t>& base,
                                const std::string& output_directory,
                                const char* name, std::uint16_t flags,
                                std::uint64_t region_offset,
                                std::uint64_t region_length) {
  std::vector<std::uint8_t> image = base;
  const std::size_t header = static_cast<std::size_t>(kVolumePrefixSize);
  const std::size_t descriptor = header + 128 + 4 * 40;
  put16(&image, header + 88, 5);
  std::fill(image.begin() + descriptor, image.begin() + descriptor + 40, 0);
  put16(&image, descriptor, 0x7001);
  put16(&image, descriptor + 2, flags);
  put16(&image, descriptor + 8, 1);
  put64(&image, descriptor + 24, region_offset);
  put64(&image, descriptor + 32, region_length);
  repair_crc(&image, header, 1024, 104);
  return write_image(joined(output_directory, name), image);
}

bool generate_mutations(const std::string& base_path,
                        const std::string& output_directory) {
  std::vector<std::uint8_t> base;
  if (!read_image(base_path, &base) || base.size() != 139264) {
    std::cerr << "unexpected valid-mixed corpus size\n";
    return false;
  }
  if (!write_unknown_feature_case(base, output_directory,
          "feature-optional-metadata.tfdb", 0, 0, 0) ||
      !write_unknown_feature_case(base, output_directory,
          "feature-required-unknown.tfdb", 1, 0, 0) ||
      !write_unknown_feature_case(base, output_directory,
          "feature-optional-region.tfdb", 0, 4096, 512) ||
      !write_unknown_feature_case(base, output_directory,
          "bounds-feature-region-overflow.tfdb", 0,
          std::numeric_limits<std::uint64_t>::max() - 255, 512))
    return false;

  std::vector<std::uint8_t> geometry = base;
  const std::uint64_t huge_aligned_partition =
      std::numeric_limits<std::uint64_t>::max() - 511;
  for (const std::size_t header : {std::size_t(0), std::size_t(4096)}) {
    put64(&geometry, header + 40, huge_aligned_partition);
    repair_crc(&geometry, header, 256, 72);
  }
  return write_image(joined(output_directory,
                            "bounds-volume-partition-size.tfdb"), geometry);
}

bool format_memory(const std::shared_ptr<tfdb::MemoryStorage>& storage,
                   std::uint64_t volume_id_high,
                   std::uint64_t volume_id_low) {
  tfdb::VolumeOptions volume;
  volume.partition_size = kPartitionSize;
  volume.index_region_size = 4096;
  volume.max_block_payload = 256;
  volume.persistence_quantum = 512;
  volume.volume_id_high = volume_id_high;
  volume.volume_id_low = volume_id_low;
  volume.allow_explicit_volume_id_for_testing = true;
  return check(tfdb::RingStore::format(*storage, volume));
}

bool open_writer(const std::shared_ptr<tfdb::MemoryStorage>& storage,
                 std::uint64_t writer_id_high,
                 std::uint64_t writer_id_low,
                 std::unique_ptr<tfdb::RingStore>* store) {
  tfdb::OpenOptions options;
  options.writable = true;
  options.writer_id_high = writer_id_high;
  options.writer_id_low = writer_id_low;
  options.allow_explicit_writer_id_for_testing = true;
  return check(tfdb::RingStore::open(storage, options, store));
}

bool append_block(tfdb::RingStore* store, std::uint8_t value,
                  std::int64_t time) {
  const std::vector<std::uint8_t> payload(200, value);
  return check(store->append(tfdb::ByteView(payload), time)) &&
         check(store->checkpoint());
}

bool generate_stale_writer_chain(const std::string& output_directory) {
  const std::uint64_t size = kVolumePrefixSize + 3 * kPartitionSize;
  std::shared_ptr<tfdb::MemoryStorage> storage(new tfdb::MemoryStorage(size));
  if (!format_memory(storage, 0x4142434445464748ull,
                     0x5152535455565758ull))
    return false;

  std::unique_ptr<tfdb::RingStore> store;
  if (!open_writer(storage, 0x1111, 0x2222, &store) ||
      !append_block(store.get(), 'A', 1))
    return false;

  storage->reset_counters();
  const std::vector<std::uint8_t> torn(200, 'B');
  const std::vector<std::uint8_t> stale(200, 'C');
  if (!check(store->append(tfdb::ByteView(torn), 2)) ||
      !check(store->publish()) ||
      !check(store->append(tfdb::ByteView(stale), 3)) ||
      !check(store->publish()) ||
      !check(storage->crash_materialize({{1, 0, 64}, {2, 0, 328}})))
    return false;
  store.reset();

  if (!open_writer(storage, 0x3333, 0x4444, &store) ||
      !append_block(store.get(), 'D', 4))
    return false;
  store.reset();
  return write_image(joined(output_directory, "stale-writer-chain.tfdb"),
                     storage->durable_image());
}

bool generate_live_rotation(const std::string& output_directory) {
  const std::uint64_t size = kVolumePrefixSize + 2 * kPartitionSize;
  std::shared_ptr<tfdb::MemoryStorage> storage(new tfdb::MemoryStorage(size));
  if (!format_memory(storage, 0x6162636465666768ull,
                     0x7172737475767778ull))
    return false;
  std::unique_ptr<tfdb::RingStore> store;
  if (!open_writer(storage, 0x8182, 0x8384, &store) ||
      !append_block(store.get(), 'A', 1) ||
      !check(store->rotate(tfdb::PartitionOptions())) ||
      !append_block(store.get(), 'B', 2) ||
      !write_image(joined(output_directory,
                          "live-rotation-01-before-reuse.tfdb"),
                   storage->durable_image()) ||
      !check(store->rotate(tfdb::PartitionOptions())) ||
      !write_image(joined(output_directory,
                          "live-rotation-02-header-reused.tfdb"),
                   storage->durable_image()) ||
      !append_block(store.get(), 'C', 3) ||
      !write_image(joined(output_directory,
                          "live-rotation-03-new-block.tfdb"),
                   storage->durable_image()))
    return false;
  return true;
}

// A partition configured for lz4_block:1 that exercises the decoder paths a
// reader has to get right: an extended literal run, extended and short match
// lengths, an overlapping distance-one run, and a block the codec could not
// shrink, which therefore stores itself as `none` inside a compressed
// partition.
bool generate_lz4_block_codec(const std::string& output_directory) {
  const std::uint64_t size = kVolumePrefixSize + 2 * kPartitionSize;
  std::shared_ptr<tfdb::MemoryStorage> storage(new tfdb::MemoryStorage(size));
  tfdb::VolumeOptions volume;
  volume.partition_size = kPartitionSize;
  volume.index_region_size = 4096;
  volume.max_block_payload = 4096;
  volume.persistence_quantum = 512;
  volume.volume_id_high = 0x4c5a34424c4b3031ull;
  volume.volume_id_low = 0x0102030405060708ull;
  volume.allow_explicit_volume_id_for_testing = true;
  volume.created_time_ns = -7;
  if (!check(tfdb::RingStore::format(*storage, volume))) return false;

  tfdb::PartitionOptions options;
  options.compression = tfdb::CompressionId::lz4_block;
  options.compression_version = 1;
  options.time_domain_id = 44;

  tfdb::OpenOptions open;
  open.writable = true;
  open.next_partition = options;
  open.writer_id_high = 0x6162636465666768ull;
  open.writer_id_low = 0x7172737475767778ull;
  open.allow_explicit_writer_id_for_testing = true;
  std::unique_ptr<tfdb::RingStore> store;
  if (!check(tfdb::RingStore::open(storage, open, &store))) return false;

  // A single-byte run: one literal, then one long distance-one match.
  std::vector<std::uint8_t> run(2000, 0x2a);
  // Fifteen or more leading literals force the extended literal length, and
  // the four-byte cycle that follows gives a long distance-four match.
  std::vector<std::uint8_t> extended;
  for (unsigned i = 0; i != 40; ++i)
    extended.push_back(static_cast<std::uint8_t>(i * 7 + 1));
  for (unsigned i = 0; i != 1200; ++i)
    extended.push_back(static_cast<std::uint8_t>(i % 4));
  // A sixteen-byte cycle whose value shifts every sixty-four bytes produces
  // many sequences with short and medium match lengths.
  std::vector<std::uint8_t> cycles(1500);
  for (std::size_t i = 0; i != cycles.size(); ++i)
    cycles[i] = static_cast<std::uint8_t>((i % 16) + (i / 64));
  // A deterministic pseudo-random payload the codec cannot shrink. The block
  // must fall back to `none` rather than pay for the larger form.
  std::vector<std::uint8_t> noise(1500);
  std::uint32_t state = 0x1234567u;
  for (std::uint8_t& byte : noise) {
    state = state * 1103515245u + 12345u;
    byte = static_cast<std::uint8_t>(state >> 16);
  }

  const std::vector<std::uint8_t>* payloads[4] = {&run, &extended, &cycles,
                                                  &noise};
  for (unsigned i = 0; i != 4; ++i) {
    if (!check(store->append(tfdb::ByteView(*payloads[i]),
                             static_cast<std::int64_t>(i) * 100)) ||
        !check(store->checkpoint()))
      return false;
  }
  if (!check(store->close())) return false;
  store.reset();
  const std::vector<std::uint8_t> image = storage->durable_image();
  if (!write_image(joined(output_directory, "codec-lz4-block.tfdb"), image))
    return false;

  // The same volume with the compression feature claiming lz4_block:2. Both
  // readers must call that unsupported at open rather than assume version 1
  // is close enough.
  std::vector<std::uint8_t> unknown_version = image;
  const std::size_t header = static_cast<std::size_t>(kVolumePrefixSize);
  put16(&unknown_version, header + 128 + 8, 2);
  repair_crc(&unknown_version, header, 1024, 104);
  return write_image(joined(output_directory, "codec-unknown-version.tfdb"),
                     unknown_version);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: tfdb_make_conformance_cases VALID_MIXED OUTPUT_DIR\n";
    return 2;
  }
  if (!generate_mutations(argv[1], argv[2]) ||
      !generate_stale_writer_chain(argv[2]) ||
      !generate_live_rotation(argv[2]) ||
      !generate_lz4_block_codec(argv[2]))
    return 1;
  return 0;
}

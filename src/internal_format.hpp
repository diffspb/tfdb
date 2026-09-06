#ifndef TFDB_INTERNAL_FORMAT_HPP
#define TFDB_INTERNAL_FORMAT_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include "tfdb/bytes.hpp"
#include "tfdb/ring_store.hpp"
#include "tfdb/status.hpp"

namespace tfdb {
namespace internal {

constexpr std::uint16_t kFormatMajor = 1;
constexpr std::uint16_t kFormatMinor = 0;
constexpr std::uint32_t kVolumeHeaderCopySize = 4096;
constexpr std::uint32_t kVolumeHeaderEncodedSize = 256;
constexpr std::uint64_t kVolumePrefixSize = 8192;
constexpr std::uint32_t kPartitionHeaderRegionSize = 4096;
constexpr std::uint32_t kPartitionHeaderEncodedSize = 1024;
constexpr std::uint32_t kPartitionFooterRegionSize = 4096;
constexpr std::uint32_t kPartitionFooterEncodedSize = 256;
constexpr std::uint32_t kBlockHeaderEncodedSize = 128;
constexpr std::uint32_t kIndexEntryEncodedSize = 48;
constexpr std::uint32_t kFeatureDescriptorSize = 40;

constexpr std::uint16_t kFeatureRequired = 1u << 0;
constexpr std::uint16_t kFeatureCompression = 1;
constexpr std::uint16_t kFeatureTimeIndex = 2;
constexpr std::uint16_t kFeatureRecordProfile = 3;
constexpr std::uint16_t kFeatureIntegrity = 4;
constexpr std::uint32_t kIntegrityCrc32c = 1;
constexpr std::uint32_t kTimeIndexMinMax = 1;

struct VolumeHeader {
  std::uint64_t volume_id_high = 0;
  std::uint64_t volume_id_low = 0;
  std::uint64_t volume_size = 0;
  std::uint64_t partition_size = 0;
  std::uint32_t partition_count = 0;
  std::uint32_t index_region_size = 0;
  std::uint32_t max_block_payload = 0;
  std::uint32_t persistence_quantum = 0;
  std::int64_t created_time_ns = 0;
};

struct FeatureDescriptor {
  std::uint16_t kind = 0;
  std::uint16_t flags = 0;
  std::uint32_t algorithm = 0;
  std::uint16_t version = 0;
  std::uint32_t config_offset = 0;
  std::uint32_t config_length = 0;
  std::uint64_t region_offset = 0;
  std::uint64_t region_length = 0;
};

struct PartitionHeader {
  std::uint64_t volume_id_high = 0;
  std::uint64_t volume_id_low = 0;
  std::uint64_t generation = 0;
  std::uint32_t slot = 0;
  std::int64_t created_time_ns = 0;
  std::uint32_t max_block_payload = 0;
  std::uint32_t persistence_quantum = 0;
  PartitionOptions options;
  std::vector<FeatureDescriptor> features;
};

struct BlockHeader {
  std::uint64_t generation = 0;
  std::uint32_t slot = 0;
  std::uint32_t sequence = 0;
  std::uint32_t flags = 0;
  std::uint32_t record_count = 0;
  std::uint32_t stored_size = 0;
  std::uint32_t raw_size = 0;
  std::uint32_t frame_span = 0;
  std::int64_t min_time_ns = 0;
  std::int64_t max_time_ns = 0;
  CompressionId compression = CompressionId::none;
  std::uint16_t compression_version = 1;
  std::uint32_t payload_crc = 0;
  std::uint64_t volume_id_low = 0;
  std::uint64_t volume_id_high = 0;
  // A random ID is generated for each writable RingStore incarnation. The
  // previous ID chains adjacent blocks and prevents a stale same-generation
  // suffix from becoming valid after recovery overwrites a torn block.
  std::uint64_t writer_id_high = 0;
  std::uint64_t writer_id_low = 0;
  std::uint64_t previous_writer_id_high = 0;
  std::uint64_t previous_writer_id_low = 0;
};

struct IndexEntry {
  std::uint64_t offset = 0;  // Relative to partition start.
  std::uint32_t frame_size = 0;
  std::uint32_t frame_span = 0;
  std::uint32_t sequence = 0;
  std::uint32_t record_count = 0;
  std::int64_t min_time_ns = 0;
  std::int64_t max_time_ns = 0;
  std::uint32_t flags = 0;
  std::uint32_t raw_size = 0;
  // Reconstructed/writer-only chain state; not serialized in the v1 index.
  std::uint64_t writer_id_high = 0;
  std::uint64_t writer_id_low = 0;
};

struct PartitionFooter {
  std::uint64_t volume_id_high = 0;
  std::uint64_t volume_id_low = 0;
  std::uint64_t generation = 0;
  std::uint32_t slot = 0;
  std::uint32_t flags = 0;
  std::uint32_t block_count = 0;
  std::uint64_t index_offset = 0;
  std::uint32_t index_size = 0;
  std::uint64_t data_end = 0;
  std::int64_t min_time_ns = 0;
  std::int64_t max_time_ns = 0;
  std::uint32_t index_crc = 0;
};

// CRC32C (Castagnoli), reflected polynomial 0x82f63b78. crc32c() is the
// one-shot form. The update/finish pair lets a caller checksum a structure
// whose own CRC field must read as zero without copying the structure first:
//   state = crc32c_update(kCrc32cInit, before_field);
//   state = crc32c_update(state, four_zero_bytes);
//   state = crc32c_update(state, after_field);
//   value = crc32c_finish(state);
constexpr std::uint32_t kCrc32cInit = 0xffffffffu;
std::uint32_t crc32c_update(std::uint32_t state, ByteView bytes);
std::uint32_t crc32c_finish(std::uint32_t state);
std::uint32_t crc32c(ByteView bytes);
// Checksums bytes[0, size) with the four bytes at crc_offset treated as zero.
// The caller must have established crc_offset + 4 <= bytes.size(); this helper
// reads the whole span and does not re-check it.
std::uint32_t crc32c_with_zeroed_field(ByteView bytes, std::size_t crc_offset);
std::uint64_t align_up(std::uint64_t value, std::uint32_t quantum,
                       bool* overflow = nullptr);

std::vector<std::uint8_t> encode_volume_header(const VolumeHeader& header);
Status decode_volume_header(ByteView bytes, VolumeHeader* header);

std::vector<std::uint8_t> encode_partition_header(
    const PartitionHeader& header, std::uint64_t partition_size,
    std::uint32_t index_region_size);
Status decode_partition_header(ByteView bytes, PartitionHeader* header);

std::vector<std::uint8_t> encode_block_header(const BlockHeader& header);
Status decode_block_header(ByteView bytes, BlockHeader* header);

void encode_index_entry(const IndexEntry& entry, std::uint8_t* output);
Status decode_index_entry(ByteView bytes, IndexEntry* entry);
std::vector<std::uint8_t> encode_index(const std::vector<IndexEntry>& entries);
Status decode_index(ByteView bytes, std::vector<IndexEntry>* entries);

std::vector<std::uint8_t> encode_partition_footer(
    const PartitionFooter& footer);
Status decode_partition_footer(ByteView bytes, PartitionFooter* footer);

}  // namespace internal
}  // namespace tfdb

#endif  // TFDB_INTERNAL_FORMAT_HPP

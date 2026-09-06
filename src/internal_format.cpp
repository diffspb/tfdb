#include "internal_format.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

namespace tfdb {
namespace internal {
namespace {

const std::uint8_t kVolumeMagic[8] = {'T','F','D','B','V','O','L','1'};
const std::uint8_t kPartitionMagic[8] = {'T','F','D','B','P','A','R','1'};
const std::uint8_t kBlockMagic[8] = {'T','F','D','B','B','L','K','1'};
const std::uint8_t kFooterMagic[8] = {'T','F','D','B','F','T','R','1'};

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
void puti64(std::uint8_t* out, std::int64_t value) {
  put64(out, static_cast<std::uint64_t>(value));
}
std::uint16_t get16(const std::uint8_t* in) {
  return static_cast<std::uint16_t>(in[0]) |
         static_cast<std::uint16_t>(in[1]) << 8;
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
std::int64_t geti64(const std::uint8_t* in) {
  const std::uint64_t value = get64(in);
  if (value <= static_cast<std::uint64_t>(INT64_MAX))
    return static_cast<std::int64_t>(value);
  return -1 - static_cast<std::int64_t>(~value);
}

bool magic_is(ByteView bytes, const std::uint8_t magic[8]) {
  return bytes.size() >= 8 && std::memcmp(bytes.data(), magic, 8) == 0;
}

bool all_zero(const std::uint8_t* bytes, std::size_t size) {
  for (std::size_t i = 0; i != size; ++i)
    if (bytes[i] != 0) return false;
  return true;
}

Status common_check(ByteView bytes, const std::uint8_t magic[8],
                    std::uint32_t expected_size, std::size_t crc_offset) {
  if (bytes.size() < expected_size) {
    return Status::Error(StatusCode::corrupt, "truncated structure");
  }
  if (!magic_is(bytes, magic)) {
    return Status::Error(StatusCode::corrupt, "structure magic mismatch");
  }
  if (get16(bytes.data() + 8) != kFormatMajor) {
    return Status::Error(StatusCode::unsupported, "unsupported format major");
  }
  if (get16(bytes.data() + 10) > kFormatMinor) {
    return Status::Error(StatusCode::unsupported, "unsupported format minor");
  }
  const std::uint32_t encoded_size = get32(bytes.data() + 12);
  if (encoded_size != expected_size || encoded_size > bytes.size()) {
    return Status::Error(StatusCode::corrupt, "structure size mismatch");
  }
  if (crc_offset + 4 > encoded_size) {
    return Status::Error(StatusCode::internal_error,
                         "structure CRC field outside its own encoding");
  }
  const std::uint32_t expected_crc = get32(bytes.data() + crc_offset);
  if (crc32c_with_zeroed_field(ByteView(bytes.data(), encoded_size),
                               crc_offset) != expected_crc) {
    return Status::Error(StatusCode::corrupt, "structure CRC mismatch");
  }
  return Status::Ok();
}

void finish_crc(std::vector<std::uint8_t>* bytes, std::size_t offset) {
  put32(bytes->data() + offset, 0);
  put32(bytes->data() + offset, crc32c(ByteView(*bytes)));
}

}  // namespace

std::uint32_t crc32c_update(std::uint32_t state, ByteView bytes) {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> result{};
    for (std::uint32_t i = 0; i != 256; ++i) {
      std::uint32_t value = i;
      for (unsigned bit = 0; bit != 8; ++bit)
        value = (value >> 1) ^ ((value & 1u) ? 0x82f63b78u : 0u);
      result[i] = value;
    }
    return result;
  }();
  for (std::size_t i = 0; i != bytes.size(); ++i)
    state = table[(state ^ bytes.data()[i]) & 0xffu] ^ (state >> 8);
  return state;
}

std::uint32_t crc32c_finish(std::uint32_t state) { return state ^ 0xffffffffu; }

std::uint32_t crc32c(ByteView bytes) {
  return crc32c_finish(crc32c_update(kCrc32cInit, bytes));
}

std::uint32_t crc32c_with_zeroed_field(ByteView bytes, std::size_t crc_offset) {
  static const std::uint8_t zero[4] = {0, 0, 0, 0};
  const std::uint8_t* data = bytes.data();
  std::uint32_t state = crc32c_update(kCrc32cInit, ByteView(data, crc_offset));
  state = crc32c_update(state, ByteView(zero, sizeof zero));
  return crc32c_finish(crc32c_update(
      state, ByteView(data + crc_offset + 4,
                      bytes.size() - crc_offset - 4)));
}

std::uint64_t align_up(std::uint64_t value, std::uint32_t quantum,
                       bool* overflow) {
  if (overflow) *overflow = false;
  if (quantum == 0) {
    if (overflow) *overflow = true;
    return 0;
  }
  const std::uint64_t remainder = value % quantum;
  if (remainder == 0) return value;
  const std::uint64_t add = quantum - remainder;
  if (value > std::numeric_limits<std::uint64_t>::max() - add) {
    if (overflow) *overflow = true;
    return 0;
  }
  return value + add;
}

std::vector<std::uint8_t> encode_volume_header(const VolumeHeader& h) {
  std::vector<std::uint8_t> out(kVolumeHeaderEncodedSize, 0);
  std::copy(kVolumeMagic, kVolumeMagic + 8, out.begin());
  put16(out.data() + 8, kFormatMajor); put16(out.data() + 10, kFormatMinor);
  put32(out.data() + 12, kVolumeHeaderEncodedSize);
  put64(out.data() + 16, h.volume_id_high); put64(out.data() + 24, h.volume_id_low);
  put64(out.data() + 32, h.volume_size); put64(out.data() + 40, h.partition_size);
  put32(out.data() + 48, h.partition_count); put32(out.data() + 52, h.index_region_size);
  put32(out.data() + 56, h.max_block_payload); put32(out.data() + 60, h.persistence_quantum);
  puti64(out.data() + 64, h.created_time_ns);
  finish_crc(&out, 72);
  return out;
}

Status decode_volume_header(ByteView bytes, VolumeHeader* h) {
  if (!h) return Status::Error(StatusCode::invalid_argument, "null volume header");
  Status status = common_check(bytes, kVolumeMagic, kVolumeHeaderEncodedSize, 72);
  if (!status.ok()) return status;
  if (!all_zero(bytes.data() + 76, kVolumeHeaderEncodedSize - 76))
    return Status::Error(StatusCode::corrupt,
                         "volume header reserved bytes are nonzero");
  h->volume_id_high = get64(bytes.data() + 16); h->volume_id_low = get64(bytes.data() + 24);
  h->volume_size = get64(bytes.data() + 32); h->partition_size = get64(bytes.data() + 40);
  h->partition_count = get32(bytes.data() + 48); h->index_region_size = get32(bytes.data() + 52);
  h->max_block_payload = get32(bytes.data() + 56); h->persistence_quantum = get32(bytes.data() + 60);
  h->created_time_ns = geti64(bytes.data() + 64);
  if (h->partition_count < 2 || h->partition_size == 0 ||
      h->max_block_payload == 0 || h->persistence_quantum == 0) {
    return Status::Error(StatusCode::corrupt, "invalid volume geometry");
  }
  return Status::Ok();
}

std::vector<std::uint8_t> encode_partition_header(
    const PartitionHeader& h, std::uint64_t partition_size,
    std::uint32_t index_region_size) {
  std::vector<std::uint8_t> out(kPartitionHeaderEncodedSize, 0);
  std::copy(kPartitionMagic, kPartitionMagic + 8, out.begin());
  put16(out.data() + 8, kFormatMajor); put16(out.data() + 10, kFormatMinor);
  put32(out.data() + 12, kPartitionHeaderEncodedSize);
  put64(out.data() + 16, h.volume_id_high); put64(out.data() + 24, h.volume_id_low);
  put64(out.data() + 32, h.generation); put32(out.data() + 40, h.slot);
  puti64(out.data() + 48, h.created_time_ns);
  put32(out.data() + 56, h.max_block_payload); put32(out.data() + 60, h.persistence_quantum);
  puti64(out.data() + 64, h.options.allowed_backward_skew_ns);
  puti64(out.data() + 72, h.options.allowed_forward_step_ns);
  put64(out.data() + 80, h.options.time_domain_id);
  put16(out.data() + 88, 4); put16(out.data() + 90, kFeatureDescriptorSize);
  put32(out.data() + 92, 128); put32(out.data() + 96, 512); put32(out.data() + 100, 16);

  const std::uint64_t index_offset = partition_size - kPartitionFooterRegionSize - index_region_size;
  FeatureDescriptor descriptors[4];
  descriptors[0] = {kFeatureCompression, kFeatureRequired,
                    static_cast<std::uint32_t>(h.options.compression),
                    h.options.compression_version, 0, 0, 0, 0};
  descriptors[1] = {kFeatureTimeIndex, kFeatureRequired, kTimeIndexMinMax, 1, 0, 0,
                    index_offset, index_region_size};
  descriptors[2] = {kFeatureRecordProfile, 0, 0,
                    h.options.record_format_version, 512, 8, 0, 0};
  descriptors[3] = {kFeatureIntegrity, kFeatureRequired, kIntegrityCrc32c,
                    1, 0, 0, 0, 0};
  for (unsigned i = 0; i != 4; ++i) {
    std::uint8_t* d = out.data() + 128 + i * kFeatureDescriptorSize;
    put16(d, descriptors[i].kind); put16(d + 2, descriptors[i].flags);
    put32(d + 4, descriptors[i].algorithm); put16(d + 8, descriptors[i].version);
    put32(d + 12, descriptors[i].config_offset); put32(d + 16, descriptors[i].config_length);
    put64(d + 24, descriptors[i].region_offset); put64(d + 32, descriptors[i].region_length);
  }
  put64(out.data() + 512, h.options.record_format_id);
  finish_crc(&out, 104);
  return out;
}

Status decode_partition_header(ByteView bytes, PartitionHeader* h) {
  if (!h) return Status::Error(StatusCode::invalid_argument, "null partition header");
  Status status = common_check(bytes, kPartitionMagic, kPartitionHeaderEncodedSize, 104);
  if (!status.ok()) return status;
  if (!all_zero(bytes.data() + 44, 4) ||
      !all_zero(bytes.data() + 108, 20))
    return Status::Error(StatusCode::corrupt,
                         "partition header reserved bytes are nonzero");
  h->volume_id_high = get64(bytes.data() + 16); h->volume_id_low = get64(bytes.data() + 24);
  h->generation = get64(bytes.data() + 32); h->slot = get32(bytes.data() + 40);
  h->created_time_ns = geti64(bytes.data() + 48);
  h->max_block_payload = get32(bytes.data() + 56); h->persistence_quantum = get32(bytes.data() + 60);
  h->options.allowed_backward_skew_ns = geti64(bytes.data() + 64);
  h->options.allowed_forward_step_ns = geti64(bytes.data() + 72);
  h->options.time_domain_id = get64(bytes.data() + 80);
  const std::uint16_t count = get16(bytes.data() + 88);
  const std::uint16_t descriptor_size = get16(bytes.data() + 90);
  const std::uint32_t descriptor_offset = get32(bytes.data() + 92);
  const std::uint32_t config_area_offset = get32(bytes.data() + 96);
  const std::uint32_t config_area_length = get32(bytes.data() + 100);
  if (count > 8 || descriptor_size != kFeatureDescriptorSize ||
      descriptor_offset != 128 || config_area_offset != 512 ||
      config_area_length != 16 ||
      static_cast<std::uint64_t>(count) * descriptor_size >
          kPartitionHeaderEncodedSize - descriptor_offset) {
    return Status::Error(StatusCode::corrupt, "invalid feature directory");
  }
  h->features.clear();
  bool compression = false, time_index = false, integrity = false;
  bool record_profile = false;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> config_ranges;
  for (std::uint16_t i = 0; i != count; ++i) {
    const std::uint8_t* d = bytes.data() + descriptor_offset + i * descriptor_size;
    FeatureDescriptor feature;
    feature.kind = get16(d); feature.flags = get16(d + 2); feature.algorithm = get32(d + 4);
    feature.version = get16(d + 8); feature.config_offset = get32(d + 12);
    feature.config_length = get32(d + 16); feature.region_offset = get64(d + 24);
    feature.region_length = get64(d + 32);
    if ((feature.flags & ~kFeatureRequired) != 0 ||
        !all_zero(d + 10, 2) || !all_zero(d + 20, 4))
      return Status::Error(StatusCode::corrupt,
                           "feature descriptor reserved fields are nonzero");
    if (feature.config_length == 0) {
      if (feature.config_offset != 0)
        return Status::Error(StatusCode::corrupt,
                             "empty feature config has nonzero offset");
    } else {
      const std::uint32_t config_area_end = config_area_offset +
                                            config_area_length;
      if (feature.config_offset < config_area_offset ||
          feature.config_offset > config_area_end ||
          feature.config_length > config_area_end - feature.config_offset)
        return Status::Error(StatusCode::corrupt,
                             "feature config outside configuration area");
      const std::uint32_t feature_end = feature.config_offset +
                                        feature.config_length;
      for (const std::pair<std::uint32_t, std::uint32_t>& range : config_ranges)
        if (feature.config_offset < range.second && range.first < feature_end)
          return Status::Error(StatusCode::corrupt,
                               "overlapping feature configurations");
      config_ranges.push_back({feature.config_offset, feature_end});
    }
    if (feature.kind == kFeatureCompression) {
      if (compression) return Status::Error(StatusCode::corrupt, "duplicate compression feature");
      compression = true;
      if ((feature.flags & kFeatureRequired) == 0 || feature.version == 0 ||
          feature.algorithm > UINT16_MAX ||
          feature.config_offset != 0 || feature.config_length != 0 ||
          feature.region_offset != 0 || feature.region_length != 0)
        return Status::Error(StatusCode::corrupt,
                             "invalid compression feature descriptor");
      h->options.compression = static_cast<CompressionId>(feature.algorithm);
      h->options.compression_version = feature.version;
    } else if (feature.kind == kFeatureTimeIndex) {
      if (time_index) return Status::Error(StatusCode::corrupt, "duplicate time-index feature");
      time_index = true;
      if ((feature.flags & kFeatureRequired) == 0 ||
          feature.config_offset != 0 || feature.config_length != 0 ||
          feature.region_length == 0)
        return Status::Error(StatusCode::corrupt,
                             "invalid time-index feature descriptor");
      if (feature.algorithm != kTimeIndexMinMax || feature.version != 1)
        return Status::Error(StatusCode::unsupported, "unsupported time index");
    } else if (feature.kind == kFeatureRecordProfile) {
      if (record_profile) return Status::Error(StatusCode::corrupt, "duplicate record-profile feature");
      record_profile = true;
      if (feature.flags != 0 || feature.algorithm != 0 ||
          feature.config_offset != 512 || feature.config_length != 8 ||
          feature.region_offset != 0 || feature.region_length != 0)
        return Status::Error(StatusCode::corrupt,
                             "invalid record-profile feature descriptor");
      h->options.record_format_version = feature.version;
      h->options.record_format_id = get64(bytes.data() + feature.config_offset);
    } else if (feature.kind == kFeatureIntegrity) {
      if (integrity) return Status::Error(StatusCode::corrupt, "duplicate integrity feature");
      integrity = true;
      if ((feature.flags & kFeatureRequired) == 0 ||
          feature.config_offset != 0 || feature.config_length != 0 ||
          feature.region_offset != 0 || feature.region_length != 0)
        return Status::Error(StatusCode::corrupt,
                             "invalid integrity feature descriptor");
      if (feature.algorithm != kIntegrityCrc32c || feature.version != 1)
        return Status::Error(StatusCode::unsupported, "unsupported integrity feature");
    } else if (feature.flags & kFeatureRequired) {
      return Status::Error(StatusCode::unsupported, "unknown required feature");
    }
    h->features.push_back(feature);
  }
  if (!compression || !time_index || !integrity || !record_profile) {
    return Status::Error(StatusCode::corrupt, "missing core feature");
  }
  if ((h->options.record_format_id == 0) !=
      (h->options.record_format_version == 0))
    return Status::Error(StatusCode::corrupt,
                         "record profile ID/version mismatch");
  return Status::Ok();
}

std::vector<std::uint8_t> encode_block_header(const BlockHeader& h) {
  std::vector<std::uint8_t> out(kBlockHeaderEncodedSize, 0);
  std::copy(kBlockMagic, kBlockMagic + 8, out.begin());
  put16(out.data() + 8, kFormatMajor); put16(out.data() + 10, kFormatMinor);
  put32(out.data() + 12, kBlockHeaderEncodedSize); put64(out.data() + 16, h.generation);
  put32(out.data() + 24, h.sequence); put32(out.data() + 28, h.flags);
  put32(out.data() + 32, h.record_count); put32(out.data() + 36, h.stored_size);
  put32(out.data() + 40, h.raw_size); put32(out.data() + 44, h.frame_span);
  puti64(out.data() + 48, h.min_time_ns); puti64(out.data() + 56, h.max_time_ns);
  put16(out.data() + 64, static_cast<std::uint16_t>(h.compression));
  put16(out.data() + 66, h.compression_version); put32(out.data() + 68, h.payload_crc);
  put32(out.data() + 76, h.slot); put64(out.data() + 80, h.volume_id_low);
  put64(out.data() + 88, h.volume_id_high);
  put64(out.data() + 96, h.writer_id_high);
  put64(out.data() + 104, h.writer_id_low);
  put64(out.data() + 112, h.previous_writer_id_high);
  put64(out.data() + 120, h.previous_writer_id_low);
  finish_crc(&out, 72);
  return out;
}

Status decode_block_header(ByteView bytes, BlockHeader* h) {
  if (!h) return Status::Error(StatusCode::invalid_argument, "null block header");
  Status status = common_check(bytes, kBlockMagic, kBlockHeaderEncodedSize, 72);
  if (!status.ok()) return status;
  h->generation = get64(bytes.data() + 16); h->sequence = get32(bytes.data() + 24);
  h->flags = get32(bytes.data() + 28); h->record_count = get32(bytes.data() + 32);
  h->stored_size = get32(bytes.data() + 36); h->raw_size = get32(bytes.data() + 40);
  h->frame_span = get32(bytes.data() + 44); h->min_time_ns = geti64(bytes.data() + 48);
  h->max_time_ns = geti64(bytes.data() + 56);
  h->compression = static_cast<CompressionId>(get16(bytes.data() + 64));
  h->compression_version = get16(bytes.data() + 66); h->payload_crc = get32(bytes.data() + 68);
  h->slot = get32(bytes.data() + 76); h->volume_id_low = get64(bytes.data() + 80);
  h->volume_id_high = get64(bytes.data() + 88);
  h->writer_id_high = get64(bytes.data() + 96);
  h->writer_id_low = get64(bytes.data() + 104);
  h->previous_writer_id_high = get64(bytes.data() + 112);
  h->previous_writer_id_low = get64(bytes.data() + 120);
  if (h->stored_size == 0 || h->raw_size == 0 || h->record_count == 0 ||
      (h->writer_id_high == 0 && h->writer_id_low == 0) ||
      h->min_time_ns > h->max_time_ns ||
      h->frame_span < kBlockHeaderEncodedSize + h->stored_size) {
    return Status::Error(StatusCode::corrupt, "invalid block bounds");
  }
  return Status::Ok();
}

void encode_index_entry(const IndexEntry& e, std::uint8_t* out) {
  put64(out, e.offset); put32(out + 8, e.frame_size); put32(out + 12, e.frame_span);
  put32(out + 16, e.sequence); put32(out + 20, e.record_count);
  puti64(out + 24, e.min_time_ns); puti64(out + 32, e.max_time_ns);
  put32(out + 40, e.flags); put32(out + 44, e.raw_size);
}

Status decode_index_entry(ByteView bytes, IndexEntry* e) {
  if (!e || bytes.size() < kIndexEntryEncodedSize)
    return Status::Error(StatusCode::corrupt, "truncated index entry");
  e->offset = get64(bytes.data()); e->frame_size = get32(bytes.data() + 8);
  e->frame_span = get32(bytes.data() + 12); e->sequence = get32(bytes.data() + 16);
  e->record_count = get32(bytes.data() + 20); e->min_time_ns = geti64(bytes.data() + 24);
  e->max_time_ns = geti64(bytes.data() + 32); e->flags = get32(bytes.data() + 40);
  e->raw_size = get32(bytes.data() + 44);
  if (e->frame_size < kBlockHeaderEncodedSize || e->frame_size > e->frame_span ||
      e->record_count == 0 || e->min_time_ns > e->max_time_ns)
    return Status::Error(StatusCode::corrupt, "invalid index entry");
  return Status::Ok();
}

std::vector<std::uint8_t> encode_index(const std::vector<IndexEntry>& entries) {
  std::vector<std::uint8_t> out(entries.size() * kIndexEntryEncodedSize, 0);
  for (std::size_t i = 0; i != entries.size(); ++i)
    encode_index_entry(entries[i], out.data() + i * kIndexEntryEncodedSize);
  return out;
}

Status decode_index(ByteView bytes, std::vector<IndexEntry>* entries) {
  if (!entries || bytes.size() % kIndexEntryEncodedSize != 0)
    return Status::Error(StatusCode::corrupt, "invalid index length");
  entries->clear(); entries->reserve(bytes.size() / kIndexEntryEncodedSize);
  for (std::size_t offset = 0; offset != bytes.size(); offset += kIndexEntryEncodedSize) {
    IndexEntry entry;
    Status status = decode_index_entry(ByteView(bytes.data() + offset, kIndexEntryEncodedSize), &entry);
    if (!status.ok()) { entries->clear(); return status; }
    entries->push_back(entry);
  }
  return Status::Ok();
}

std::vector<std::uint8_t> encode_partition_footer(const PartitionFooter& f) {
  std::vector<std::uint8_t> out(kPartitionFooterEncodedSize, 0);
  std::copy(kFooterMagic, kFooterMagic + 8, out.begin());
  put16(out.data() + 8, kFormatMajor); put16(out.data() + 10, kFormatMinor);
  put32(out.data() + 12, kPartitionFooterEncodedSize);
  put64(out.data() + 16, f.volume_id_high); put64(out.data() + 24, f.volume_id_low);
  put64(out.data() + 32, f.generation); put32(out.data() + 40, f.slot);
  put32(out.data() + 44, f.flags); put32(out.data() + 48, f.block_count);
  put32(out.data() + 52, kIndexEntryEncodedSize); put64(out.data() + 56, f.index_offset);
  put32(out.data() + 64, f.index_size); put64(out.data() + 72, f.data_end);
  puti64(out.data() + 80, f.min_time_ns); puti64(out.data() + 88, f.max_time_ns);
  put32(out.data() + 96, f.index_crc);
  finish_crc(&out, 100);
  return out;
}

Status decode_partition_footer(ByteView bytes, PartitionFooter* f) {
  if (!f) return Status::Error(StatusCode::invalid_argument, "null footer");
  Status status = common_check(bytes, kFooterMagic, kPartitionFooterEncodedSize, 100);
  if (!status.ok()) return status;
  if (!all_zero(bytes.data() + 68, 4) ||
      !all_zero(bytes.data() + 104, kPartitionFooterEncodedSize - 104))
    return Status::Error(StatusCode::corrupt,
                         "footer reserved bytes are nonzero");
  f->volume_id_high = get64(bytes.data() + 16); f->volume_id_low = get64(bytes.data() + 24);
  f->generation = get64(bytes.data() + 32); f->slot = get32(bytes.data() + 40);
  f->flags = get32(bytes.data() + 44); f->block_count = get32(bytes.data() + 48);
  if (get32(bytes.data() + 52) != kIndexEntryEncodedSize)
    return Status::Error(StatusCode::unsupported, "unsupported index entry size");
  f->index_offset = get64(bytes.data() + 56); f->index_size = get32(bytes.data() + 64);
  f->data_end = get64(bytes.data() + 72); f->min_time_ns = geti64(bytes.data() + 80);
  f->max_time_ns = geti64(bytes.data() + 88); f->index_crc = get32(bytes.data() + 96);
  if (f->index_size != static_cast<std::uint64_t>(f->block_count) * kIndexEntryEncodedSize ||
      (f->block_count != 0 && f->min_time_ns > f->max_time_ns))
    return Status::Error(StatusCode::corrupt, "invalid footer bounds");
  return Status::Ok();
}

}  // namespace internal
}  // namespace tfdb

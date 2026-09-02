#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "internal_format.hpp"
#include "tfdb/async_writer.hpp"
#include "tfdb/codec.hpp"
#include "tfdb/memory_storage.hpp"
#include "tfdb/record_profile.hpp"
#include "tfdb/ring_store.hpp"
#include "tfdb/storage.hpp"

namespace {

struct Failure {
  std::string message;
};

using Test = std::pair<const char*, std::function<void()>>;
std::vector<Test>& tests() { static std::vector<Test> value; return value; }

std::uint8_t hex_nibble(char value) {
  if (value >= '0' && value <= '9') return static_cast<std::uint8_t>(value - '0');
  if (value >= 'a' && value <= 'f')
    return static_cast<std::uint8_t>(value - 'a' + 10);
  throw Failure{"invalid golden-vector hex"};
}

std::vector<std::uint8_t> from_hex(const char* text) {
  const std::size_t length = std::strlen(text);
  if (length % 2 != 0) throw Failure{"odd golden-vector hex length"};
  std::vector<std::uint8_t> bytes(length / 2);
  for (std::size_t i = 0; i != bytes.size(); ++i)
    bytes[i] = static_cast<std::uint8_t>((hex_nibble(text[i * 2]) << 4) |
                                         hex_nibble(text[i * 2 + 1]));
  return bytes;
}

void golden_put16(std::vector<std::uint8_t>* bytes, std::size_t offset,
                  std::uint16_t value) {
  for (unsigned i = 0; i != 2; ++i)
    (*bytes)[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}
void golden_put32(std::vector<std::uint8_t>* bytes, std::size_t offset,
                  std::uint32_t value) {
  for (unsigned i = 0; i != 4; ++i)
    (*bytes)[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}
void golden_put64(std::vector<std::uint8_t>* bytes, std::size_t offset,
                  std::uint64_t value) {
  for (unsigned i = 0; i != 8; ++i)
    (*bytes)[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}
void golden_magic(std::vector<std::uint8_t>* bytes, const char* magic) {
  std::copy(magic, magic + 8, bytes->begin());
}

struct Register {
  Register(const char* name, const std::function<void()>& function) {
    tests().push_back(Test(name, function));
  }
};

#define TEST(name) void name(); Register reg_##name(#name, name); void name()
#define REQUIRE(value) do { if (!(value)) { std::ostringstream out; out << __FILE__ << ':' << __LINE__ << ": requirement failed: " #value; throw Failure{out.str()}; } } while (0)
#define REQUIRE_EQ(a,b) do { const auto value_a=(a); const auto value_b=(b); if (!(value_a == value_b)) { std::ostringstream out; out << __FILE__ << ':' << __LINE__ << ": expected " #a " == " #b << " (" << value_a << " vs " << value_b << ')'; throw Failure{out.str()}; } } while (0)
#define REQUIRE_OK(value) do { const tfdb::Status status=(value); if (!status.ok()) { std::ostringstream out; out << __FILE__ << ':' << __LINE__ << ": status " << tfdb::status_code_name(status.code()) << ": " << status.message(); throw Failure{out.str()}; } } while (0)

struct Fixture {
  std::shared_ptr<tfdb::MemoryStorage> storage;
  std::unique_ptr<tfdb::RingStore> store;
  tfdb::VolumeOptions volume;
  tfdb::PartitionOptions partition;

  Fixture(std::uint64_t partition_size = 64u * 1024u,
          std::uint32_t max_payload = 256,
          std::uint32_t quantum = 512,
          std::uint32_t partition_count = 3) {
    volume.partition_size = partition_size;
    volume.index_region_size = 4096;
    volume.max_block_payload = max_payload;
    volume.persistence_quantum = quantum;
    volume.volume_id_high = 0x1111222233334444ull;
    volume.volume_id_low = 0x5555666677778888ull;
    volume.allow_explicit_volume_id_for_testing = true;
    const std::uint64_t size = tfdb::internal::kVolumePrefixSize +
                               partition_size * partition_count;
    storage.reset(new tfdb::MemoryStorage(size));
    REQUIRE_OK(tfdb::RingStore::format(*storage, volume));
  }

  void open(bool writable = true) {
    tfdb::OpenOptions options;
    options.writable = writable;
    options.next_partition = partition;
    REQUIRE_OK(tfdb::RingStore::open(storage, options, &store));
  }
};

class RepeatedByteCodec final : public tfdb::CompressionCodec {
 public:
  tfdb::CompressionId id() const override {
    return static_cast<tfdb::CompressionId>(100);
  }
  std::uint16_t version() const override { return 7; }
  std::size_t max_compressed_size(std::size_t input_size) const override {
    return input_size;
  }
  tfdb::Status compress(tfdb::ByteView input,
                        std::vector<std::uint8_t>* output) const override {
    if (!output || input.empty())
      return tfdb::Status::Error(tfdb::StatusCode::invalid_argument,
                                 "invalid repeated-byte input/output");
    for (std::size_t i = 1; i != input.size(); ++i)
      if (input.data()[i] != input.data()[0]) {
        output->assign(input.data(), input.data() + input.size());
        return tfdb::Status::Ok();
      }
    output->assign(1, input.data()[0]);
    return tfdb::Status::Ok();
  }
  tfdb::Status decompress(tfdb::ByteView input, std::size_t expected_size,
                          std::vector<std::uint8_t>* output) const override {
    if (!output || input.size() != 1 || expected_size == 0)
      return tfdb::Status::Error(tfdb::StatusCode::corrupt,
                                 "invalid repeated-byte block");
    output->assign(expected_size, input.data()[0]);
    return tfdb::Status::Ok();
  }
};

class RejectingRepeatedByteDecoder final : public tfdb::CompressionCodec {
 public:
  tfdb::CompressionId id() const override {
    return static_cast<tfdb::CompressionId>(100);
  }
  std::uint16_t version() const override { return 7; }
  std::size_t max_compressed_size(std::size_t input_size) const override {
    return input_size;
  }
  tfdb::Status compress(tfdb::ByteView,
                        std::vector<std::uint8_t>*) const override {
    return tfdb::Status::Error(tfdb::StatusCode::internal_error,
                               "decoder-only test codec");
  }
  tfdb::Status decompress(tfdb::ByteView, std::size_t,
                          std::vector<std::uint8_t>*) const override {
    return tfdb::Status::Error(tfdb::StatusCode::corrupt,
                               "injected compressed syntax failure");
  }
};

std::vector<std::vector<std::uint8_t>> collect_blocks(const tfdb::RingStore& store) {
  std::vector<std::vector<std::uint8_t>> result;
  tfdb::QueryOptions options;
  REQUIRE_OK(store.scan_blocks(options, [&](const tfdb::BlockEvent& event) {
    if (event.kind != tfdb::BlockEventKind::data)
      throw Failure{"unexpected block gap: " + event.detail.message()};
    result.emplace_back(event.data.data(), event.data.data() + event.data.size());
    return true;
  }));
  return result;
}

std::vector<std::vector<std::uint8_t>> collect_blocks_with_gaps(
    const tfdb::RingStore& store, std::size_t expected_gaps) {
  std::vector<std::vector<std::uint8_t>> result;
  std::size_t gaps = 0;
  REQUIRE_OK(store.scan_blocks(tfdb::QueryOptions(),
      [&](const tfdb::BlockEvent& event) {
        if (event.kind == tfdb::BlockEventKind::data)
          result.emplace_back(event.data.data(),
                              event.data.data() + event.data.size());
        else
          ++gaps;
        return true;
      }));
  REQUIRE_EQ(gaps, expected_gaps);
  return result;
}

std::vector<std::uint64_t> collect_selectors(const tfdb::RingStore& store,
                                             std::int64_t begin,
                                             std::int64_t end,
                                             const std::vector<std::uint64_t>& filter = {}) {
  tfdb::FramedRecordV1 profile;
  tfdb::RecordQuery query;
  query.time.begin_ns = begin;
  query.time.end_ns = end;
  query.selectors = filter;
  std::vector<std::uint64_t> result;
  REQUIRE_OK(tfdb::query_records(store, query, profile,
      [&](const tfdb::RecordEvent& event) {
        if (event.kind != tfdb::BlockEventKind::data)
          throw Failure{"unexpected record gap: " + event.detail.message()};
        result.push_back(event.record.selector);
        return true;
      }));
  return result;
}

TEST(crc32c_known_vector) {
  const char text[] = "123456789";
  REQUIRE_EQ(tfdb::internal::crc32c(tfdb::ByteView(text, 9)), 0xe3069283u);
}

TEST(all_status_codes_have_stable_names) {
  const tfdb::StatusCode codes[] = {
      tfdb::StatusCode::ok, tfdb::StatusCode::invalid_argument,
      tfdb::StatusCode::out_of_range, tfdb::StatusCode::io_error,
      tfdb::StatusCode::interrupted, tfdb::StatusCode::no_space,
      tfdb::StatusCode::corrupt, tfdb::StatusCode::unsupported,
      tfdb::StatusCode::not_found, tfdb::StatusCode::busy,
      tfdb::StatusCode::overwritten, tfdb::StatusCode::generation_exhausted,
      tfdb::StatusCode::closed, tfdb::StatusCode::internal_error};
  for (tfdb::StatusCode code : codes)
    REQUIRE(std::string(tfdb::status_code_name(code)) != "unknown");
  REQUIRE(std::string(tfdb::status_code_name(
      static_cast<tfdb::StatusCode>(999))) == "unknown");
}

TEST(format_v1_normative_golden_vectors) {
  tfdb::internal::VolumeHeader volume;
  volume.volume_id_high = 0x0102030405060708ull;
  volume.volume_id_low = 0x1112131415161718ull;
  volume.volume_size = 0x102000;
  volume.partition_size = 0x80000;
  volume.partition_count = 2;
  volume.index_region_size = 4096;
  volume.max_block_payload = 32768;
  volume.persistence_quantum = 4096;
  volume.created_time_ns = -2;
  std::vector<std::uint8_t> expected_volume(256, 0);
  golden_magic(&expected_volume, "TFDBVOL1");
  golden_put16(&expected_volume, 8, 1);
  golden_put32(&expected_volume, 12, 256);
  golden_put64(&expected_volume, 16, volume.volume_id_high);
  golden_put64(&expected_volume, 24, volume.volume_id_low);
  golden_put64(&expected_volume, 32, volume.volume_size);
  golden_put64(&expected_volume, 40, volume.partition_size);
  golden_put32(&expected_volume, 48, volume.partition_count);
  golden_put32(&expected_volume, 52, volume.index_region_size);
  golden_put32(&expected_volume, 56, volume.max_block_payload);
  golden_put32(&expected_volume, 60, volume.persistence_quantum);
  golden_put64(&expected_volume, 64, UINT64_MAX - 1);
  const std::uint8_t volume_crc[] = {0x4c, 0xee, 0x84, 0x05};
  std::copy(volume_crc, volume_crc + 4, expected_volume.begin() + 72);
  REQUIRE(tfdb::internal::encode_volume_header(volume) == expected_volume);

  tfdb::internal::PartitionHeader partition;
  partition.volume_id_high = volume.volume_id_high;
  partition.volume_id_low = volume.volume_id_low;
  partition.generation = 7;
  partition.slot = 1;
  partition.created_time_ns = -3;
  partition.max_block_payload = volume.max_block_payload;
  partition.persistence_quantum = volume.persistence_quantum;
  partition.options.compression = tfdb::CompressionId::packbits;
  partition.options.compression_version = 1;
  partition.options.record_format_id = tfdb::kFramedRecordV1ProfileId;
  partition.options.record_format_version = 1;
  partition.options.time_domain_id = 2;
  partition.options.allowed_backward_skew_ns = 10;
  partition.options.allowed_forward_step_ns = 20;
  std::vector<std::uint8_t> expected_partition(1024, 0);
  golden_magic(&expected_partition, "TFDBPAR1");
  golden_put16(&expected_partition, 8, 1);
  golden_put32(&expected_partition, 12, 1024);
  golden_put64(&expected_partition, 16, volume.volume_id_high);
  golden_put64(&expected_partition, 24, volume.volume_id_low);
  golden_put64(&expected_partition, 32, 7);
  golden_put32(&expected_partition, 40, 1);
  golden_put64(&expected_partition, 48, UINT64_MAX - 2);
  golden_put32(&expected_partition, 56, 32768);
  golden_put32(&expected_partition, 60, 4096);
  golden_put64(&expected_partition, 64, 10);
  golden_put64(&expected_partition, 72, 20);
  golden_put64(&expected_partition, 80, 2);
  golden_put16(&expected_partition, 88, 4);
  golden_put16(&expected_partition, 90, 40);
  golden_put32(&expected_partition, 92, 128);
  golden_put32(&expected_partition, 96, 512);
  golden_put32(&expected_partition, 100, 16);
  const std::uint8_t partition_crc[] = {0xa9, 0xac, 0xea, 0xd7};
  std::copy(partition_crc, partition_crc + 4,
            expected_partition.begin() + 104);
  // compression, min/max time index, record profile, CRC32C integrity
  golden_put16(&expected_partition, 128, 1); golden_put16(&expected_partition, 130, 1);
  golden_put32(&expected_partition, 132, 1); golden_put16(&expected_partition, 136, 1);
  golden_put16(&expected_partition, 168, 2); golden_put16(&expected_partition, 170, 1);
  golden_put32(&expected_partition, 172, 1); golden_put16(&expected_partition, 176, 1);
  golden_put64(&expected_partition, 192, 0x7e000); golden_put64(&expected_partition, 200, 4096);
  golden_put16(&expected_partition, 208, 3); golden_put16(&expected_partition, 216, 1);
  golden_put32(&expected_partition, 220, 512); golden_put32(&expected_partition, 224, 8);
  golden_put16(&expected_partition, 248, 4); golden_put16(&expected_partition, 250, 1);
  golden_put32(&expected_partition, 252, 1); golden_put16(&expected_partition, 256, 1);
  golden_put64(&expected_partition, 512, tfdb::kFramedRecordV1ProfileId);
  REQUIRE(tfdb::internal::encode_partition_header(partition, 0x80000, 4096) ==
          expected_partition);

  const std::vector<std::uint8_t> expected_block = from_hex(
      "54464442424c4b31010000008000000008070605040302010200000003000000"
      "04000000050000000600000000020000feffffffffffffff0900000000000000"
      "01000100443322114c9b18220d0c0b0a80706050403020101122334455667788"
      "0202020201010101040404040303030306060606050505050808080807070707");
  tfdb::internal::BlockHeader block;
  block.generation = 0x0102030405060708ull; block.slot = 0x0a0b0c0d;
  block.sequence = 2; block.flags = 3; block.record_count = 4;
  block.stored_size = 5; block.raw_size = 6; block.frame_span = 512;
  block.min_time_ns = -2; block.max_time_ns = 9;
  block.compression = tfdb::CompressionId::packbits;
  block.compression_version = 1; block.payload_crc = 0x11223344;
  block.volume_id_low = 0x1020304050607080ull;
  block.volume_id_high = 0x8877665544332211ull;
  block.writer_id_high = 0x0101010102020202ull;
  block.writer_id_low = 0x0303030304040404ull;
  block.previous_writer_id_high = 0x0505050506060606ull;
  block.previous_writer_id_low = 0x0707070708080808ull;
  REQUIRE(tfdb::internal::encode_block_header(block) == expected_block);

  tfdb::internal::IndexEntry index;
  index.offset = 4096; index.frame_size = 133; index.frame_span = 512;
  index.sequence = 2; index.record_count = 4; index.min_time_ns = -2;
  index.max_time_ns = 9; index.flags = 3; index.raw_size = 6;
  std::vector<std::uint8_t> index_bytes(48, 0);
  tfdb::internal::encode_index_entry(index, index_bytes.data());
  REQUIRE(index_bytes == from_hex(
      "001000000000000085000000000200000200000004000000feffffffffffffff"
      "09000000000000000300000006000000"));

  tfdb::internal::PartitionFooter footer;
  footer.volume_id_high = volume.volume_id_high;
  footer.volume_id_low = volume.volume_id_low;
  footer.generation = 7; footer.slot = 1; footer.flags = 3;
  footer.block_count = 1; footer.index_offset = 0x7e000;
  footer.index_size = 48; footer.data_end = 4608;
  footer.min_time_ns = -2; footer.max_time_ns = 9;
  footer.index_crc = 0x12345678;
  std::vector<std::uint8_t> expected_footer(256, 0);
  golden_magic(&expected_footer, "TFDBFTR1");
  golden_put16(&expected_footer, 8, 1); golden_put32(&expected_footer, 12, 256);
  golden_put64(&expected_footer, 16, volume.volume_id_high);
  golden_put64(&expected_footer, 24, volume.volume_id_low);
  golden_put64(&expected_footer, 32, 7); golden_put32(&expected_footer, 40, 1);
  golden_put32(&expected_footer, 44, 3); golden_put32(&expected_footer, 48, 1);
  golden_put32(&expected_footer, 52, 48); golden_put64(&expected_footer, 56, 0x7e000);
  golden_put32(&expected_footer, 64, 48); golden_put64(&expected_footer, 72, 4608);
  golden_put64(&expected_footer, 80, UINT64_MAX - 1); golden_put64(&expected_footer, 88, 9);
  golden_put32(&expected_footer, 96, 0x12345678);
  const std::uint8_t footer_crc[] = {0xba, 0xbb, 0x5b, 0x38};
  std::copy(footer_crc, footer_crc + 4, expected_footer.begin() + 100);
  REQUIRE(tfdb::internal::encode_partition_footer(footer) == expected_footer);

  std::vector<std::uint8_t> framed;
  const char framed_payload[] = {'l', 'o', 'g'};
  REQUIRE_OK(tfdb::FramedRecordV1::encode(
      std::numeric_limits<std::int64_t>::min(), 0x1122334455667788ull, 7,
      tfdb::ByteView(framed_payload, sizeof framed_payload), &framed));
  REQUIRE(framed == from_hex(
      "23000000200007000000000000000080887766554433221103000000e8c1198a"
      "6c6f67"));
}

TEST(persistent_decoders_reject_truncation_and_survive_mutation) {
  tfdb::internal::BlockHeader block;
  block.generation = 1; block.slot = 0; block.record_count = 1;
  block.stored_size = 1; block.raw_size = 1; block.frame_span = 512;
  block.min_time_ns = -1; block.max_time_ns = 1;
  block.volume_id_high = 1; block.volume_id_low = 2;
  block.writer_id_high = 3; block.writer_id_low = 4;
  const std::vector<std::uint8_t> original =
      tfdb::internal::encode_block_header(block);
  for (std::size_t length = 0; length != original.size(); ++length) {
    tfdb::internal::BlockHeader decoded;
    REQUIRE(!tfdb::internal::decode_block_header(
        tfdb::ByteView(original.data(), length), &decoded).ok());
  }
  std::mt19937 random(0xdec0de);
  for (unsigned trial = 0; trial != 10000; ++trial) {
    std::vector<std::uint8_t> mutated = original;
    const std::size_t offset = random() % mutated.size();
    mutated[offset] ^= static_cast<std::uint8_t>(1u << (random() % 8));
    tfdb::internal::BlockHeader decoded;
    const tfdb::Status status = tfdb::internal::decode_block_header(
        tfdb::ByteView(mutated), &decoded);
    REQUIRE(!status.ok());
  }
  const std::shared_ptr<const tfdb::CompressionCodec> codec =
      tfdb::packbits_codec();
  for (unsigned trial = 0; trial != 10000; ++trial) {
    const std::size_t size = random() % 64;
    std::vector<std::uint8_t> bytes(size);
    for (std::uint8_t& byte : bytes)
      byte = static_cast<std::uint8_t>(random());
    std::vector<std::uint8_t> output;
    const tfdb::Status status = codec->decompress(
        tfdb::ByteView(bytes), random() % 256, &output);
    if (status.ok()) REQUIRE(output.size() <= 255);
  }
}

TEST(all_persistent_decoders_have_truncation_and_crc_mutation_coverage) {
  tfdb::internal::VolumeHeader volume;
  volume.volume_id_high = 1; volume.volume_id_low = 2;
  volume.volume_size = tfdb::internal::kVolumePrefixSize + 2u * 65536u;
  volume.partition_size = 65536;
  volume.partition_count = 2;
  volume.index_region_size = 4096;
  volume.max_block_payload = 256;
  volume.persistence_quantum = 512;
  const std::vector<std::uint8_t> volume_bytes =
      tfdb::internal::encode_volume_header(volume);

  tfdb::internal::PartitionHeader partition;
  partition.volume_id_high = 1; partition.volume_id_low = 2;
  partition.generation = 1; partition.slot = 0;
  partition.max_block_payload = 256;
  partition.persistence_quantum = 512;
  const std::vector<std::uint8_t> partition_bytes =
      tfdb::internal::encode_partition_header(partition, 65536, 4096);

  tfdb::internal::BlockHeader block;
  block.generation = 1; block.slot = 0; block.record_count = 1;
  block.stored_size = 1; block.raw_size = 1; block.frame_span = 512;
  block.writer_id_high = 3; block.writer_id_low = 4;
  block.volume_id_high = 1; block.volume_id_low = 2;
  const std::vector<std::uint8_t> block_bytes =
      tfdb::internal::encode_block_header(block);

  tfdb::internal::PartitionFooter footer;
  footer.volume_id_high = 1; footer.volume_id_low = 2;
  footer.generation = 1; footer.slot = 0;
  footer.index_offset = 57344; footer.data_end = 4096;
  const std::vector<std::uint8_t> footer_bytes =
      tfdb::internal::encode_partition_footer(footer);

  const auto exercise_crc_structure = [](
      const std::vector<std::uint8_t>& bytes,
      const std::function<tfdb::Status(tfdb::ByteView)>& decode) {
    for (std::size_t length = 0; length != bytes.size(); ++length)
      REQUIRE(!decode(tfdb::ByteView(bytes.data(), length)).ok());
    for (std::size_t offset = 0; offset != bytes.size(); ++offset) {
      std::vector<std::uint8_t> mutated = bytes;
      mutated[offset] ^= 1;
      REQUIRE(!decode(tfdb::ByteView(mutated)).ok());
    }
    REQUIRE_OK(decode(tfdb::ByteView(bytes)));
  };
  exercise_crc_structure(volume_bytes, [](tfdb::ByteView bytes) {
    tfdb::internal::VolumeHeader value;
    return tfdb::internal::decode_volume_header(bytes, &value);
  });
  exercise_crc_structure(partition_bytes, [](tfdb::ByteView bytes) {
    tfdb::internal::PartitionHeader value;
    return tfdb::internal::decode_partition_header(bytes, &value);
  });
  exercise_crc_structure(block_bytes, [](tfdb::ByteView bytes) {
    tfdb::internal::BlockHeader value;
    return tfdb::internal::decode_block_header(bytes, &value);
  });
  exercise_crc_structure(footer_bytes, [](tfdb::ByteView bytes) {
    tfdb::internal::PartitionFooter value;
    return tfdb::internal::decode_partition_footer(bytes, &value);
  });

  tfdb::internal::IndexEntry index;
  index.offset = 4096; index.frame_size = 129; index.frame_span = 512;
  index.record_count = 1;
  std::vector<std::uint8_t> index_bytes(
      tfdb::internal::kIndexEntryEncodedSize);
  tfdb::internal::encode_index_entry(index, index_bytes.data());
  for (std::size_t length = 0; length != index_bytes.size(); ++length) {
    tfdb::internal::IndexEntry decoded;
    REQUIRE(!tfdb::internal::decode_index_entry(
        tfdb::ByteView(index_bytes.data(), length), &decoded).ok());
  }
  tfdb::internal::IndexEntry decoded_index;
  REQUIRE_OK(tfdb::internal::decode_index_entry(tfdb::ByteView(index_bytes),
                                                 &decoded_index));

  std::vector<std::uint8_t> frame;
  const char payload[] = {'a', '\0', 'b'};
  REQUIRE_OK(tfdb::FramedRecordV1::encode(1, 2, 3,
      tfdb::ByteView(payload, sizeof payload), &frame));
  tfdb::FramedRecordV1 profile;
  for (std::size_t length = 1; length != frame.size(); ++length)
    REQUIRE(!profile.decode_block(tfdb::ByteView(frame.data(), length),
        [](const tfdb::RecordView&) { return true; }).ok());
  for (std::size_t offset = 0; offset != frame.size(); ++offset) {
    std::vector<std::uint8_t> mutated = frame;
    mutated[offset] ^= 1;
    REQUIRE(!profile.decode_block(tfdb::ByteView(mutated),
        [](const tfdb::RecordView&) { return true; }).ok());
  }
}

TEST(partition_feature_directory_rejects_width_duplicates_and_profile_mismatch) {
  tfdb::internal::PartitionHeader partition;
  partition.volume_id_high = 1; partition.volume_id_low = 2;
  partition.generation = 1; partition.max_block_payload = 256;
  partition.persistence_quantum = 512;
  const auto with_crc = [](std::vector<std::uint8_t> bytes) {
    golden_put32(&bytes, 104, 0);
    golden_put32(&bytes, 104, tfdb::internal::crc32c(tfdb::ByteView(bytes)));
    return bytes;
  };
  std::vector<std::uint8_t> bytes =
      tfdb::internal::encode_partition_header(partition, 65536, 4096);
  golden_put32(&bytes, 132, 65536);
  bytes = with_crc(bytes);
  tfdb::internal::PartitionHeader decoded;
  REQUIRE(tfdb::internal::decode_partition_header(tfdb::ByteView(bytes),
                                                   &decoded).code() ==
          tfdb::StatusCode::corrupt);

  bytes = tfdb::internal::encode_partition_header(partition, 65536, 4096);
  golden_put16(&bytes, 168, tfdb::internal::kFeatureCompression);
  bytes = with_crc(bytes);
  REQUIRE(tfdb::internal::decode_partition_header(tfdb::ByteView(bytes),
                                                   &decoded).code() ==
          tfdb::StatusCode::corrupt);

  bytes = tfdb::internal::encode_partition_header(partition, 65536, 4096);
  golden_put32(&bytes, 92, 129);
  bytes = with_crc(bytes);
  REQUIRE(tfdb::internal::decode_partition_header(tfdb::ByteView(bytes),
                                                   &decoded).code() ==
          tfdb::StatusCode::corrupt);

  bytes = tfdb::internal::encode_partition_header(partition, 65536, 4096);
  golden_put32(&bytes, 96, 520);
  bytes = with_crc(bytes);
  REQUIRE(tfdb::internal::decode_partition_header(tfdb::ByteView(bytes),
                                                   &decoded).code() ==
          tfdb::StatusCode::corrupt);

  bytes = tfdb::internal::encode_partition_header(partition, 65536, 4096);
  golden_put32(&bytes, 220, 520);
  bytes = with_crc(bytes);
  REQUIRE(tfdb::internal::decode_partition_header(tfdb::ByteView(bytes),
                                                   &decoded).code() ==
          tfdb::StatusCode::corrupt);

  partition.options.record_format_version = 1;
  bytes = tfdb::internal::encode_partition_header(partition, 65536, 4096);
  REQUIRE(tfdb::internal::decode_partition_header(tfdb::ByteView(bytes),
                                                   &decoded).code() ==
          tfdb::StatusCode::corrupt);
}

TEST(packbits_round_trip_and_bounds) {
  const std::shared_ptr<const tfdb::CompressionCodec> codec = tfdb::packbits_codec();
  std::vector<std::uint8_t> input;
  input.insert(input.end(), 300, 0);
  for (unsigned i = 0; i != 256; ++i) input.push_back(static_cast<std::uint8_t>(i));
  input.insert(input.end(), 17, 42);
  std::vector<std::uint8_t> compressed, decoded;
  REQUIRE_OK(codec->compress(tfdb::ByteView(input), &compressed));
  REQUIRE(compressed.size() < input.size());
  REQUIRE_OK(codec->decompress(tfdb::ByteView(compressed), input.size(), &decoded));
  REQUIRE(input == decoded);
  compressed.push_back(0x80);
  REQUIRE(!codec->decompress(tfdb::ByteView(compressed), input.size(), &decoded).ok());
}

TEST(public_codecs_and_profiles_reject_invalid_byte_views) {
  const tfdb::ByteView invalid(static_cast<const void*>(nullptr), 1);
  std::vector<std::uint8_t> output;
  const std::shared_ptr<const tfdb::CompressionCodec> none =
      tfdb::no_compression_codec();
  const std::shared_ptr<const tfdb::CompressionCodec> packbits =
      tfdb::packbits_codec();
  REQUIRE(none->compress(invalid, &output).code() ==
          tfdb::StatusCode::invalid_argument);
  REQUIRE(none->decompress(invalid, 1, &output).code() ==
          tfdb::StatusCode::invalid_argument);
  REQUIRE(packbits->compress(invalid, &output).code() ==
          tfdb::StatusCode::invalid_argument);
  REQUIRE(packbits->decompress(invalid, 1, &output).code() ==
          tfdb::StatusCode::invalid_argument);
  REQUIRE(tfdb::FramedRecordV1::encode(0, 0, 0, invalid, &output).code() ==
          tfdb::StatusCode::invalid_argument);
  tfdb::FramedRecordV1 profile;
  REQUIRE(profile.decode_block(
      invalid, [](const tfdb::RecordView&) { return true; }).code() ==
      tfdb::StatusCode::invalid_argument);
}

TEST(public_encoders_support_aliased_input_and_output) {
  const std::shared_ptr<const tfdb::CompressionCodec> none =
      tfdb::no_compression_codec();
  std::vector<std::uint8_t> raw = {1, 2, 3, 4};
  REQUIRE_OK(none->compress(tfdb::ByteView(raw), &raw));
  REQUIRE(raw == std::vector<std::uint8_t>({1, 2, 3, 4}));
  REQUIRE_OK(none->decompress(tfdb::ByteView(raw), raw.size(), &raw));
  REQUIRE(raw == std::vector<std::uint8_t>({1, 2, 3, 4}));

  const std::shared_ptr<const tfdb::CompressionCodec> packbits =
      tfdb::packbits_codec();
  raw.assign(130, 0x5a);
  const std::vector<std::uint8_t> expected_raw = raw;
  REQUIRE_OK(packbits->compress(tfdb::ByteView(raw), &raw));
  REQUIRE(raw == std::vector<std::uint8_t>({0xff, 0x5a}));
  REQUIRE_OK(packbits->decompress(tfdb::ByteView(raw), expected_raw.size(),
                                  &raw));
  REQUIRE(raw == expected_raw);

  std::vector<std::uint8_t> framed_payload = {'a', 'l', 'i', 'a', 's'};
  const std::vector<std::uint8_t> expected_payload = framed_payload;
  REQUIRE_OK(tfdb::FramedRecordV1::encode(
      -5, 42, 7, tfdb::ByteView(framed_payload), &framed_payload));
  tfdb::FramedRecordV1 profile;
  std::vector<std::uint8_t> decoded_payload;
  REQUIRE_OK(profile.decode_block(
      tfdb::ByteView(framed_payload), [&](const tfdb::RecordView& record) {
        decoded_payload.assign(record.payload.data(),
                               record.payload.data() + record.payload.size());
        return true;
      }));
  REQUIRE(decoded_payload == expected_payload);
}

TEST(packbits_v1_normative_and_malformed_vectors) {
  const std::shared_ptr<const tfdb::CompressionCodec> codec =
      tfdb::packbits_codec();
  const auto encodes_as = [&](const std::vector<std::uint8_t>& input,
                              const std::vector<std::uint8_t>& expected) {
    std::vector<std::uint8_t> encoded, decoded;
    REQUIRE_OK(codec->compress(tfdb::ByteView(input), &encoded));
    REQUIRE(encoded == expected);
    REQUIRE_OK(codec->decompress(tfdb::ByteView(encoded), input.size(),
                                 &decoded));
    REQUIRE(decoded == input);
  };
  encodes_as({}, {});
  encodes_as({1, 2}, {1, 1, 2});
  encodes_as({0xaa, 0xaa, 0xaa}, {0x80, 0xaa});
  encodes_as({0x41, 0x41, 0x42, 0x42, 0x42, 0x43},
             {1, 0x41, 0x41, 0x80, 0x42, 0, 0x43});
  encodes_as(std::vector<std::uint8_t>(130, 0x7e), {0xff, 0x7e});
  encodes_as(std::vector<std::uint8_t>(131, 0x7e),
             {0xff, 0x7e, 0, 0x7e});

  const auto rejects = [&](const std::vector<std::uint8_t>& input,
                           std::size_t expected_size) {
    std::vector<std::uint8_t> output;
    REQUIRE(codec->decompress(tfdb::ByteView(input), expected_size,
                              &output).code() == tfdb::StatusCode::corrupt);
  };
  rejects({0}, 1);
  rejects({2, 1, 2}, 3);
  rejects({0x80}, 3);
  rejects({0x80, 1}, 2);
  rejects({0, 1, 0, 2}, 1);
}

TEST(format_headers_and_geometry_round_trip) {
  Fixture fixture;
  fixture.open(false);
  tfdb::VolumeInfo info;
  REQUIRE_OK(fixture.store->inspect(&info));
  REQUIRE_EQ(info.partition_count, 3u);
  REQUIRE_EQ(info.partition_size, fixture.volume.partition_size);
  REQUIRE_EQ(info.persistence_quantum, 512u);
  REQUIRE(info.partitions.empty());
}

TEST(format_accepts_exactly_one_quantum_data_region) {
  tfdb::VolumeOptions volume;
  volume.persistence_quantum = 256;
  volume.index_region_size = 256;
  volume.max_block_payload = 1;
  volume.partition_size = tfdb::internal::kPartitionHeaderRegionSize +
                          tfdb::internal::kPartitionFooterRegionSize +
                          volume.index_region_size +
                          volume.persistence_quantum;
  volume.volume_id_high = 1;
  volume.volume_id_low = 2;
  volume.allow_explicit_volume_id_for_testing = true;
  const std::uint64_t total_size = tfdb::internal::kVolumePrefixSize +
                                   2 * volume.partition_size;
  std::shared_ptr<tfdb::MemoryStorage> storage(
      new tfdb::MemoryStorage(total_size));
  REQUIRE_OK(tfdb::RingStore::format(*storage, volume));
  tfdb::OpenOptions open;
  open.writable = true;
  std::unique_ptr<tfdb::RingStore> store;
  REQUIRE_OK(tfdb::RingStore::open(storage, open, &store));
  const std::uint8_t first = 0x11;
  const std::uint8_t second = 0x22;
  REQUIRE_OK(store->append(tfdb::ByteView(&first, 1), 1));
  REQUIRE_OK(store->checkpoint());
  REQUIRE_OK(store->append(tfdb::ByteView(&second, 1), 2));
  REQUIRE_OK(store->checkpoint());
  REQUIRE_OK(store->close());
  store.reset();
  storage->crash_discard_volatile();
  open.writable = false;
  REQUIRE_OK(tfdb::RingStore::open(storage, open, &store));
  const std::vector<std::vector<std::uint8_t>> blocks =
      collect_blocks(*store);
  REQUIRE_EQ(blocks.size(), 2u);
  REQUIRE_EQ(blocks[0].size(), 1u);
  REQUIRE_EQ(blocks[0][0], first);
  REQUIRE_EQ(blocks[1].size(), 1u);
  REQUIRE_EQ(blocks[1][0], second);
}

TEST(append_checkpoint_exact_io_and_visibility) {
  Fixture fixture;
  fixture.open();
  fixture.storage->reset_counters();
  const std::uint8_t message[] = {1, 2, 3, 4};
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(message, sizeof message), 10));
  tfdb::StorageCounters counters = fixture.storage->counters();
  REQUIRE_EQ(counters.write_calls, 0u);
  REQUIRE_EQ(counters.flush_calls, 0u);
  REQUIRE(collect_blocks(*fixture.store).empty());
  REQUIRE_OK(fixture.store->checkpoint());
  counters = fixture.storage->counters();
  REQUIRE_EQ(counters.write_calls, 1u);
  REQUIRE_EQ(counters.flush_calls, 1u);
  const std::vector<tfdb::StorageEvent> all_checkpoint_events =
      fixture.storage->events();
  const auto checkpoint_event = std::find_if(
      all_checkpoint_events.begin(), all_checkpoint_events.end(),
      [](const tfdb::StorageEvent& event) {
        return event.operation == tfdb::StorageOperation::write;
      });
  REQUIRE(checkpoint_event != all_checkpoint_events.end());
  REQUIRE_EQ(checkpoint_event->requested,
             tfdb::internal::kBlockHeaderEncodedSize + sizeof message);
  REQUIRE_EQ(checkpoint_event->transferred, checkpoint_event->requested);
  const auto blocks = collect_blocks(*fixture.store);
  REQUIRE_EQ(blocks.size(), 1u);
  REQUIRE(blocks[0] == std::vector<std::uint8_t>(message, message + sizeof message));
  fixture.storage->reset_counters();
  REQUIRE_OK(fixture.store->checkpoint());
  counters = fixture.storage->counters();
  REQUIRE_EQ(counters.write_calls, 0u);
  REQUIRE_EQ(counters.flush_calls, 0u);
}

TEST(format_open_and_seal_have_exact_write_sequences) {
  const std::uint64_t partition_size = 64u * 1024u;
  std::shared_ptr<tfdb::MemoryStorage> storage(new tfdb::MemoryStorage(
      tfdb::internal::kVolumePrefixSize + partition_size * 3));
  tfdb::VolumeOptions volume;
  volume.partition_size = partition_size;
  volume.index_region_size = 4096;
  volume.max_block_payload = 256;
  volume.persistence_quantum = 512;
  volume.volume_id_high = 1;
  volume.volume_id_low = 2;
  volume.allow_explicit_volume_id_for_testing = true;
  storage->reset_counters();
  REQUIRE_OK(tfdb::RingStore::format(*storage, volume));
  std::vector<tfdb::StorageEvent> events = storage->events();
  REQUIRE_EQ(events.size(), 3u);
  REQUIRE(events[0].operation == tfdb::StorageOperation::write);
  REQUIRE_EQ(events[0].offset, 0u);
  REQUIRE_EQ(events[0].requested,
             tfdb::internal::kVolumeHeaderEncodedSize);
  REQUIRE_EQ(events[0].transferred, events[0].requested);
  REQUIRE(events[1].operation == tfdb::StorageOperation::write);
  REQUIRE_EQ(events[1].offset, tfdb::internal::kVolumeHeaderCopySize);
  REQUIRE_EQ(events[1].requested,
             tfdb::internal::kVolumeHeaderEncodedSize);
  REQUIRE_EQ(events[1].transferred, events[1].requested);
  REQUIRE(events[2].operation == tfdb::StorageOperation::flush);

  storage->reset_counters();
  tfdb::OpenOptions open;
  open.writable = true;
  std::unique_ptr<tfdb::RingStore> store;
  REQUIRE_OK(tfdb::RingStore::open(storage, open, &store));
  events = storage->events();
  REQUIRE(events.size() >= 2);
  REQUIRE(events[events.size() - 2].operation == tfdb::StorageOperation::write);
  REQUIRE_EQ(events[events.size() - 2].offset,
             tfdb::internal::kVolumePrefixSize);
  REQUIRE_EQ(events[events.size() - 2].requested,
             tfdb::internal::kPartitionHeaderEncodedSize);
  REQUIRE_EQ(events[events.size() - 2].transferred,
             events[events.size() - 2].requested);
  REQUIRE(events.back().operation == tfdb::StorageOperation::flush);

  const char value = 'w';
  REQUIRE_OK(store->append(tfdb::ByteView(&value, 1), 1));
  REQUIRE_OK(store->checkpoint());
  storage->reset_counters();
  REQUIRE_OK(store->rotate(tfdb::PartitionOptions()));
  events = storage->events();
  REQUIRE_EQ(events.size(), 5u);
  const std::uint64_t index_offset = tfdb::internal::kVolumePrefixSize +
      partition_size - tfdb::internal::kPartitionFooterRegionSize -
      volume.index_region_size;
  const std::uint64_t footer_offset = tfdb::internal::kVolumePrefixSize +
      partition_size - tfdb::internal::kPartitionFooterRegionSize;
  REQUIRE(events[0].operation == tfdb::StorageOperation::write);
  REQUIRE_EQ(events[0].offset, index_offset);
  REQUIRE_EQ(events[0].requested, tfdb::internal::kIndexEntryEncodedSize);
  REQUIRE_EQ(events[0].transferred, events[0].requested);
  REQUIRE(events[1].operation == tfdb::StorageOperation::write);
  REQUIRE_EQ(events[1].offset, footer_offset);
  REQUIRE_EQ(events[1].requested,
             tfdb::internal::kPartitionFooterEncodedSize);
  REQUIRE_EQ(events[1].transferred, events[1].requested);
  REQUIRE(events[2].operation == tfdb::StorageOperation::flush);
  REQUIRE(events[3].operation == tfdb::StorageOperation::write);
  REQUIRE_EQ(events[3].offset,
             tfdb::internal::kVolumePrefixSize + partition_size);
  REQUIRE_EQ(events[3].requested,
             tfdb::internal::kPartitionHeaderEncodedSize);
  REQUIRE_EQ(events[3].transferred, events[3].requested);
  REQUIRE(events[4].operation == tfdb::StorageOperation::flush);
}

TEST(no_record_crosses_block_boundary) {
  Fixture fixture(64u * 1024u, 10, 512);
  fixture.open();
  std::vector<std::uint8_t> a(7, 'a'), b(7, 'b');
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(a), 1));
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(b), 2));
  REQUIRE_OK(fixture.store->checkpoint());
  const auto blocks = collect_blocks(*fixture.store);
  REQUIRE_EQ(blocks.size(), 2u);
  REQUIRE(blocks[0] == a);
  REQUIRE(blocks[1] == b);
  std::vector<std::uint8_t> oversized(11, 0);
  REQUIRE(fixture.store->append(tfdb::ByteView(oversized), 3).code() == tfdb::StatusCode::out_of_range);
}

TEST(checkpointed_data_survives_and_volatile_tail_is_lost) {
  Fixture fixture;
  fixture.open();
  const char a = 'A', b = 'B';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&a, 1), 1));
  REQUIRE_OK(fixture.store->checkpoint());
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&b, 1), 2));
  REQUIRE_OK(fixture.store->publish());
  fixture.storage->crash_discard_volatile();
  fixture.store.reset();
  fixture.open(false);
  const auto blocks = collect_blocks(*fixture.store);
  REQUIRE_EQ(blocks.size(), 1u);
  REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('A'));
}

TEST(torn_tail_sweep_recovers_previous_checkpoint) {
  const std::size_t frame_bytes = tfdb::internal::kBlockHeaderEncodedSize + 1;
  for (std::size_t cut = 0; cut <= frame_bytes; ++cut) {
    Fixture fixture;
    fixture.open();
    const char a = 'A', b = 'B';
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(&a, 1), 1));
    REQUIRE_OK(fixture.store->checkpoint());
    fixture.storage->reset_counters();
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(&b, 1), 2));
    REQUIRE_OK(fixture.store->publish());
    std::vector<tfdb::WriteFragment> persisted;
    if (cut != 0) persisted.push_back({1, 0, cut});
    REQUIRE_OK(fixture.storage->crash_materialize(persisted));
    fixture.store.reset();
    fixture.open(false);
    const auto blocks = collect_blocks(*fixture.store);
    REQUIRE_EQ(blocks.size(), cut < frame_bytes ? 1u : 2u);
    REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('A'));
    if (blocks.size() == 2)
      REQUIRE_EQ(blocks[1][0], static_cast<std::uint8_t>('B'));
  }
}

TEST(compressed_block_torn_sweep_recovers_exact_prefix) {
  for (std::size_t cut = 0; cut <= 132; ++cut) {
    Fixture fixture;
    fixture.partition.compression = tfdb::CompressionId::packbits;
    fixture.open();
    const std::vector<std::uint8_t> baseline(200, 'A');
    const std::vector<std::uint8_t> pending(200, 'B');
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(baseline), 1));
    REQUIRE_OK(fixture.store->checkpoint());
    fixture.storage->reset_counters();
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(pending), 2));
    REQUIRE_OK(fixture.store->publish());
    const std::vector<tfdb::StorageEvent> events = fixture.storage->events();
    REQUIRE_EQ(events.size(), 1u);
    REQUIRE(events[0].operation == tfdb::StorageOperation::write);
    const std::size_t frame_bytes = events[0].requested;
    REQUIRE_EQ(frame_bytes, 132u);
    std::vector<tfdb::WriteFragment> persisted;
    if (cut != 0) persisted.push_back({1, 0, cut});
    REQUIRE_OK(fixture.storage->crash_materialize(persisted));
    fixture.store.reset();
    fixture.open(false);
    const auto blocks = collect_blocks(*fixture.store);
    REQUIRE_EQ(blocks.size(), cut < frame_bytes ? 1u : 2u);
    REQUIRE(blocks[0] == baseline);
    if (blocks.size() == 2) REQUIRE(blocks[1] == pending);
  }
}

TEST(multiple_dirty_blocks_subset_and_reorder_preserve_prefix) {
  // All subsets of three complete writes. A valid later block never bridges
  // a missing earlier sequence. Materialization order is separately reversed
  // for the all-complete case below.
  for (unsigned mask = 0; mask != 8; ++mask) {
    Fixture fixture;
    fixture.open();
    const std::vector<std::uint8_t> a(100, 'A');
    const std::vector<std::uint8_t> b(100, 'B');
    const std::vector<std::uint8_t> c(100, 'C');
    const std::vector<std::uint8_t> d(100, 'D');
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(a), 1));
    REQUIRE_OK(fixture.store->checkpoint());
    fixture.storage->reset_counters();
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(b), 2));
    REQUIRE_OK(fixture.store->publish());
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(c), 3));
    REQUIRE_OK(fixture.store->publish());
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(d), 4));
    REQUIRE_OK(fixture.store->publish());
    const std::vector<tfdb::StorageEvent> events = fixture.storage->events();
    REQUIRE_EQ(events.size(), 3u);
    std::vector<tfdb::WriteFragment> fragments;
    for (unsigned bit = 0; bit != 3; ++bit)
      if ((mask & (1u << bit)) != 0)
        fragments.push_back({bit + 1u, 0, events[bit].requested});
    REQUIRE_OK(fixture.storage->crash_materialize(fragments));
    fixture.store.reset();
    fixture.open(false);
    const auto blocks = collect_blocks(*fixture.store);
    std::size_t expected = 1;
    while (expected <= 3 && (mask & (1u << (expected - 1))) != 0)
      ++expected;
    REQUIRE_EQ(blocks.size(), expected);
    REQUIRE(blocks[0] == a);
    if (expected >= 2) REQUIRE(blocks[1] == b);
    if (expected >= 3) REQUIRE(blocks[2] == c);
    if (expected == 4) REQUIRE(blocks[3] == d);
  }

  enum class Special { header_only, first_and_torn_second, reverse_complete };
  for (Special special : {Special::header_only,
                          Special::first_and_torn_second,
                          Special::reverse_complete}) {
    Fixture fixture;
    fixture.open();
    const std::vector<std::uint8_t> a(100, 'A');
    const std::vector<std::uint8_t> b(100, 'B');
    const std::vector<std::uint8_t> c(100, 'C');
    const std::vector<std::uint8_t> d(100, 'D');
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(a), 1));
    REQUIRE_OK(fixture.store->checkpoint());
    fixture.storage->reset_counters();
    for (const std::vector<std::uint8_t>* payload : {&b, &c, &d}) {
      REQUIRE_OK(fixture.store->append(tfdb::ByteView(*payload), 2));
      REQUIRE_OK(fixture.store->publish());
    }
    const std::vector<tfdb::StorageEvent> events = fixture.storage->events();
    std::vector<tfdb::WriteFragment> fragments;
    if (special == Special::header_only) {
      fragments.push_back({1, 0,
                           tfdb::internal::kBlockHeaderEncodedSize});
    } else if (special == Special::first_and_torn_second) {
      fragments.push_back({1, 0, events[0].requested});
      fragments.push_back({2, 0, events[1].requested - 1});
    } else {
      for (std::size_t i = 3; i != 0; --i)
        fragments.push_back({i, 0, events[i - 1].requested});
    }
    REQUIRE_OK(fixture.storage->crash_materialize(fragments));
    fixture.store.reset();
    fixture.open(false);
    const auto blocks = collect_blocks(*fixture.store);
    const std::size_t expected = special == Special::reverse_complete ? 4u :
                                 special == Special::first_and_torn_second ? 2u : 1u;
    REQUIRE_EQ(blocks.size(), expected);
    REQUIRE(blocks[0] == a);
    if (expected >= 2) REQUIRE(blocks[1] == b);
    if (expected >= 3) REQUIRE(blocks[2] == c);
    if (expected == 4) REQUIRE(blocks[3] == d);
  }
}

TEST(failed_flush_may_persist_none_torn_or_complete_block) {
  for (unsigned outcome = 0; outcome != 3; ++outcome) {
    Fixture fixture;
    fixture.open();
    const std::vector<std::uint8_t> a(100, 'A');
    const std::vector<std::uint8_t> b(100, 'B');
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(a), 1));
    REQUIRE_OK(fixture.store->checkpoint());
    fixture.storage->reset_counters();
    fixture.storage->add_fault({tfdb::StorageOperation::flush, 1, 0,
                                tfdb::StatusCode::io_error});
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(b), 2));
    REQUIRE(fixture.store->checkpoint().code() == tfdb::StatusCode::io_error);
    const std::vector<tfdb::StorageEvent> events = fixture.storage->events();
    REQUIRE(events.size() >= 2);
    REQUIRE(events[0].operation == tfdb::StorageOperation::write);
    const std::size_t frame_size = events[0].requested;
    std::vector<tfdb::WriteFragment> fragments;
    if (outcome == 1) fragments.push_back({1, 0, frame_size - 1});
    if (outcome == 2) fragments.push_back({1, 0, frame_size});
    REQUIRE_OK(fixture.storage->crash_materialize(fragments));
    fixture.storage->clear_faults();
    fixture.store.reset();
    fixture.open(false);
    const auto blocks = collect_blocks(*fixture.store);
    REQUIRE_EQ(blocks.size(), outcome == 2 ? 2u : 1u);
    REQUIRE(blocks[0] == a);
    if (outcome == 2) REQUIRE(blocks[1] == b);
  }
}

TEST(crash_after_backend_write_before_writer_state_update_recovers_block) {
  Fixture fixture;
  fixture.open();
  const char a = 'A', b = 'B';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&a, 1), 1));
  REQUIRE_OK(fixture.store->checkpoint());
  fixture.storage->reset_counters();
  bool captured_boundary = false;
  fixture.storage->set_hook([&](tfdb::StorageOperation operation,
                                tfdb::HookPoint point, std::uint64_t call,
                                std::uint64_t, std::size_t) {
    if (operation != tfdb::StorageOperation::write ||
        point != tfdb::HookPoint::after || call != 1) return;
    fixture.storage->set_hook(tfdb::StorageHook());
    fixture.storage->crash_persist_all_dirty();
    captured_boundary = true;
  });
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&b, 1), 2));
  REQUIRE_OK(fixture.store->publish());
  REQUIRE(captured_boundary);
  // No later backend operation occurred: durable bytes are exactly the image
  // captured by HookPoint::after, before write_at() returned to RingStore.
  fixture.store.reset();
  fixture.open(false);
  const auto blocks = collect_blocks(*fixture.store);
  REQUIRE_EQ(blocks.size(), 2u);
  REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('A'));
  REQUIRE_EQ(blocks[1][0], static_cast<std::uint8_t>('B'));
}

TEST(writer_incarnation_chain_blocks_stale_suffix_after_second_crash) {
  Fixture fixture;
  tfdb::OpenOptions first_open;
  first_open.writable = true;
  first_open.next_partition = fixture.partition;
  first_open.writer_id_high = 0x1111;
  first_open.writer_id_low = 0x2222;
  first_open.allow_explicit_writer_id_for_testing = true;
  REQUIRE_OK(tfdb::RingStore::open(fixture.storage, first_open,
                                   &fixture.store));

  const std::vector<std::uint8_t> baseline(200, 'A');
  const std::vector<std::uint8_t> torn(200, 'B');
  const std::vector<std::uint8_t> stale_suffix(200, 'C');
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(baseline), 1));
  REQUIRE_OK(fixture.store->checkpoint());
  fixture.storage->reset_counters();
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(torn), 2));
  REQUIRE_OK(fixture.store->publish());
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(stale_suffix), 3));
  REQUIRE_OK(fixture.store->publish());
  const std::size_t frame_size =
      tfdb::internal::kBlockHeaderEncodedSize + torn.size();
  REQUIRE_OK(fixture.storage->crash_materialize({
      {1, 0, 64}, {2, 0, frame_size}}));
  fixture.store.reset();

  tfdb::OpenOptions second_open = first_open;
  second_open.writer_id_high = 0x3333;
  second_open.writer_id_low = 0x4444;
  REQUIRE_OK(tfdb::RingStore::open(fixture.storage, second_open,
                                   &fixture.store));
  const std::vector<std::uint8_t> replacement(200, 'D');
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(replacement), 4));
  REQUIRE_OK(fixture.store->checkpoint());
  fixture.store.reset();

  tfdb::OpenOptions reader;
  REQUIRE_OK(tfdb::RingStore::open(fixture.storage, reader, &fixture.store));
  const auto blocks = collect_blocks(*fixture.store);
  REQUIRE_EQ(blocks.size(), 2u);
  REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('A'));
  REQUIRE_EQ(blocks[1][0], static_cast<std::uint8_t>('D'));
}

TEST(compression_is_independent_and_falls_back) {
  Fixture fixture(64u * 1024u, 512, 512);
  fixture.partition.compression = tfdb::CompressionId::packbits;
  fixture.open();
  std::vector<std::uint8_t> compressible(400, 0);
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(compressible), 1));
  REQUIRE_OK(fixture.store->checkpoint());
  bool saw_compressed = false;
  REQUIRE_OK(fixture.store->scan_blocks(tfdb::QueryOptions(), [&](const tfdb::BlockEvent& event) {
    if (event.kind == tfdb::BlockEventKind::data) {
      saw_compressed = event.metadata.compression == tfdb::CompressionId::packbits;
      REQUIRE_EQ(event.data.size(), compressible.size());
      REQUIRE(std::equal(event.data.data(), event.data.data() + event.data.size(), compressible.begin()));
    }
    return true;
  }));
  REQUIRE(saw_compressed);

  std::vector<std::uint8_t> random(400);
  std::mt19937 generator(7);
  for (auto& byte : random) byte = static_cast<std::uint8_t>(generator());
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(random), 2));
  REQUIRE_OK(fixture.store->checkpoint());
  bool saw_raw = false;
  REQUIRE_OK(fixture.store->scan_blocks(tfdb::QueryOptions(), [&](const tfdb::BlockEvent& event) {
    if (event.kind == tfdb::BlockEventKind::data && event.metadata.block_sequence == 1)
      saw_raw = event.metadata.compression == tfdb::CompressionId::none;
    return true;
  }));
  REQUIRE(saw_raw);
}

TEST(custom_codec_registry_is_partition_local_and_versioned) {
  Fixture fixture(64u * 1024u, 512, 512);
  const std::shared_ptr<const tfdb::CompressionCodec> codec(
      new RepeatedByteCodec());
  tfdb::OpenOptions writable;
  writable.writable = true;
  writable.next_partition.compression = codec->id();
  writable.next_partition.compression_version = codec->version();
  writable.compression_codecs.push_back(codec);
  REQUIRE_OK(tfdb::RingStore::open(fixture.storage, writable, &fixture.store));
  std::vector<std::uint8_t> payload(400, 0xab);
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(payload), 1));
  REQUIRE_OK(fixture.store->checkpoint());
  const auto blocks = collect_blocks(*fixture.store);
  REQUIRE_EQ(blocks.size(), 1u);
  REQUIRE(blocks[0] == payload);
  fixture.store.reset();

  tfdb::OpenOptions missing;
  std::unique_ptr<tfdb::RingStore> without_codec;
  REQUIRE(tfdb::RingStore::open(fixture.storage, missing, &without_codec).code() ==
          tfdb::StatusCode::unsupported);
  tfdb::OpenOptions readable;
  readable.compression_codecs.push_back(codec);
  REQUIRE_OK(tfdb::RingStore::open(fixture.storage, readable, &fixture.store));
  REQUIRE(collect_blocks(*fixture.store)[0] == payload);
}

TEST(disordered_time_index_has_no_false_negative) {
  Fixture fixture(128u * 1024u, 70, 512);
  fixture.partition.record_format_id = tfdb::kFramedRecordV1ProfileId;
  fixture.partition.record_format_version = 1;
  fixture.partition.allowed_backward_skew_ns = 20;
  fixture.partition.allowed_forward_step_ns = 1000;
  fixture.open();
  const std::int64_t times[] = {100, 95, 105, 90, 110, 1000000, 111};
  for (std::uint64_t i = 0; i != 7; ++i) {
    const char payload = static_cast<char>('a' + i);
    REQUIRE_OK(tfdb::append_framed_record(*fixture.store, times[i], i + 1, 0,
                                         tfdb::ByteView(&payload, 1)));
  }
  REQUIRE_OK(fixture.store->checkpoint());
  for (std::uint64_t i = 0; i != 7; ++i) {
    const auto result = collect_selectors(*fixture.store, times[i], times[i] + 1);
    REQUIRE(std::find(result.begin(), result.end(), i + 1) != result.end());
  }
  REQUIRE(fixture.store->metrics().time_anomalies >= 1);
}

TEST(record_query_filters_time_and_selector_in_physical_order) {
  Fixture fixture;
  fixture.partition.record_format_id = tfdb::kFramedRecordV1ProfileId;
  fixture.partition.record_format_version = 1;
  fixture.open();
  struct Input { std::uint64_t selector; std::int64_t time; char payload; };
  const Input inputs[] = {{2, 30, 'a'}, {1, 10, 'b'},
                          {2, 20, 'c'}, {3, 40, 'd'}};
  for (const Input& input : inputs)
    REQUIRE_OK(tfdb::append_framed_record(
        *fixture.store, input.time, input.selector, 0,
        tfdb::ByteView(&input.payload, 1)));
  REQUIRE_OK(fixture.store->checkpoint());
  tfdb::FramedRecordV1 profile;
  tfdb::RecordQuery query;
  query.time = {0, 100, 1};
  query.selectors = {2};
  std::vector<char> payloads;
  REQUIRE_OK(tfdb::query_records(*fixture.store, query, profile,
      [&](const tfdb::RecordEvent& event) {
        REQUIRE(event.kind == tfdb::BlockEventKind::data);
        REQUIRE_EQ(event.record.payload.size(), 1u);
        payloads.push_back(static_cast<char>(event.record.payload.data()[0]));
        return true;
      }));
  REQUIRE(payloads == std::vector<char>({'a', 'c'}));
}

TEST(record_profile_mismatch_is_explicit) {
  Fixture fixture;
  fixture.partition.record_format_id = 123;
  fixture.partition.record_format_version = 1;
  fixture.open();
  const char byte = 1;
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&byte, 1), 1));
  REQUIRE_OK(fixture.store->checkpoint());
  tfdb::FramedRecordV1 profile;
  tfdb::RecordQuery query;
  query.time = {0, 10};
  const tfdb::Status status = tfdb::query_records(*fixture.store, query, profile,
      [](const tfdb::RecordEvent&) { return true; });
  REQUIRE(status.code() == tfdb::StatusCode::unsupported);
}

TEST(mixed_record_profiles_require_block_level_dispatch) {
  Fixture fixture;
  fixture.partition.record_format_id = tfdb::kFramedRecordV1ProfileId;
  fixture.partition.record_format_version = 1;
  fixture.open();
  const char framed_payload = 'a';
  REQUIRE_OK(tfdb::append_framed_record(
      *fixture.store, 10, 1, 0, tfdb::ByteView(&framed_payload, 1)));
  REQUIRE_OK(fixture.store->checkpoint());

  tfdb::PartitionOptions version_two = fixture.partition;
  version_two.record_format_version = 2;
  REQUIRE_OK(fixture.store->rotate(version_two));
  const char native_version_two = 'b';
  tfdb::AppendContract contract;
  contract.record_format_id = tfdb::kFramedRecordV1ProfileId;
  contract.record_format_version = 2;
  contract.time_domain_id = 1;
  REQUIRE_OK(fixture.store->append_checked(
      tfdb::ByteView(&native_version_two, 1), 20, contract));
  REQUIRE_OK(fixture.store->checkpoint());

  tfdb::RecordQuery query;
  query.time = {0, 30, 1};
  tfdb::FramedRecordV1 profile;
  std::size_t decoded_v1 = 0;
  REQUIRE(tfdb::query_records(
      *fixture.store, query, profile,
      [&](const tfdb::RecordEvent& event) {
        if (event.kind == tfdb::BlockEventKind::data) ++decoded_v1;
        return true;
      }).code() == tfdb::StatusCode::unsupported);
  REQUIRE_EQ(decoded_v1, 1u);

  std::vector<std::uint16_t> versions;
  REQUIRE_OK(fixture.store->query_blocks(
      query.time, tfdb::QueryOptions(), [&](const tfdb::BlockEvent& event) {
        REQUIRE(event.kind == tfdb::BlockEventKind::data);
        versions.push_back(event.metadata.record_format_version);
        return true;
      }));
  REQUIRE(versions == std::vector<std::uint16_t>({1, 2}));
}

TEST(rotation_reuses_slots_without_resurrecting_old_blocks) {
  Fixture fixture(16u * 1024u, 900, 1024, 2);
  fixture.volume.index_region_size = 1024;
  // Reformat with corrected smaller index region.
  REQUIRE_OK(tfdb::RingStore::format(*fixture.storage, fixture.volume));
  fixture.open();
  std::vector<std::uint8_t> payload(800, 0x5a);
  for (int i = 0; i != 30; ++i) {
    payload[0] = static_cast<std::uint8_t>(i);
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(payload), i));
    REQUIRE_OK(fixture.store->checkpoint());
  }
  tfdb::VolumeInfo info;
  REQUIRE_OK(fixture.store->inspect(&info));
  REQUIRE(info.partitions.size() <= 2);
  REQUIRE(fixture.store->metrics().rotations >= 2);
  std::vector<int> values;
  REQUIRE_OK(fixture.store->scan_blocks(tfdb::QueryOptions(), [&](const tfdb::BlockEvent& event) {
    if (event.kind == tfdb::BlockEventKind::data) values.push_back(event.data.data()[0]);
    return true;
  }));
  REQUIRE(!values.empty());
  REQUIRE(std::is_sorted(values.begin(), values.end()));
  REQUIRE(values == std::vector<int>({18, 19, 20, 21, 22, 23,
                                      24, 25, 26, 27, 28, 29}));
  REQUIRE(values.back() == 29);
}

TEST(index_reserve_rotates_exactly_at_n_plus_one_entry) {
  Fixture fixture(64u * 1024u, 256, 512, 3);
  fixture.volume.index_region_size = 512;
  REQUIRE_OK(tfdb::RingStore::format(*fixture.storage, fixture.volume));
  fixture.open();
  const char value = 'i';
  const std::size_t capacity = fixture.volume.index_region_size /
                               tfdb::internal::kIndexEntryEncodedSize;
  for (std::size_t i = 0; i != capacity; ++i) {
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1),
                                     static_cast<std::int64_t>(i)));
    REQUIRE_OK(fixture.store->checkpoint());
  }
  tfdb::VolumeInfo info;
  REQUIRE_OK(fixture.store->inspect(&info));
  REQUIRE_EQ(info.partitions.size(), 1u);
  REQUIRE_EQ(info.partitions[0].block_count, capacity);
  REQUIRE(!info.partitions[0].sealed);

  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), 100));
  REQUIRE_OK(fixture.store->checkpoint());
  REQUIRE_OK(fixture.store->inspect(&info));
  REQUIRE_EQ(info.partitions.size(), 2u);
  REQUIRE(info.partitions[0].sealed);
  REQUIRE_EQ(info.partitions[0].block_count, capacity);
  REQUIRE_EQ(info.partitions[1].block_count, 1u);
}

TEST(corrupt_sealed_index_is_rebuilt_from_blocks) {
  Fixture fixture;
  fixture.open();
  const char byte = 'x';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&byte, 1), 1));
  REQUIRE_OK(fixture.store->rotate(fixture.partition));
  tfdb::VolumeInfo info;
  REQUIRE_OK(fixture.store->inspect(&info));
  const std::uint64_t index_absolute = tfdb::internal::kVolumePrefixSize +
      fixture.volume.partition_size - tfdb::internal::kPartitionFooterRegionSize -
      fixture.volume.index_region_size;
  REQUIRE_OK(fixture.storage->corrupt_volatile(index_absolute, 0x80));
  REQUIRE_OK(fixture.storage->corrupt_durable(index_absolute, 0x80));
  fixture.store.reset();
  fixture.open(false);
  const auto blocks = collect_blocks_with_gaps(*fixture.store, 1);
  REQUIRE_EQ(blocks.size(), 1u);
  REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('x'));
}

TEST(historical_footerless_corruption_after_open_is_an_explicit_gap) {
  Fixture fixture;
  fixture.open();
  const char value = 'h';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), 10));
  REQUIRE_OK(fixture.store->checkpoint());
  REQUIRE_OK(fixture.store->rotate(fixture.partition));
  const std::uint64_t footer_offset = tfdb::internal::kVolumePrefixSize +
      fixture.volume.partition_size -
      tfdb::internal::kPartitionFooterRegionSize;
  REQUIRE_OK(fixture.storage->corrupt_volatile(footer_offset, 1));
  REQUIRE_OK(fixture.storage->corrupt_durable(footer_offset, 1));
  fixture.store.reset();
  fixture.open(false);

  const std::uint64_t payload_offset = tfdb::internal::kVolumePrefixSize +
      tfdb::internal::kPartitionHeaderRegionSize +
      tfdb::internal::kBlockHeaderEncodedSize;
  REQUIRE_OK(fixture.storage->corrupt_volatile(payload_offset, 1));
  std::size_t data_events = 0, corrupt_gaps = 0;
  REQUIRE_OK(fixture.store->scan_blocks(tfdb::QueryOptions(),
      [&](const tfdb::BlockEvent& event) {
        if (event.kind == tfdb::BlockEventKind::data) ++data_events;
        if (event.kind == tfdb::BlockEventKind::corrupt_gap) ++corrupt_gaps;
        return true;
      }));
  REQUIRE_EQ(data_events, 0u);
  REQUIRE_EQ(corrupt_gaps, 1u);
}

TEST(eager_open_verification_rejects_corrupt_sealed_payload) {
  Fixture fixture;
  fixture.open();
  const char byte = 'p';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&byte, 1), 1));
  REQUIRE_OK(fixture.store->rotate(fixture.partition));
  const std::uint64_t payload_offset = tfdb::internal::kVolumePrefixSize +
      tfdb::internal::kPartitionHeaderRegionSize +
      tfdb::internal::kBlockHeaderEncodedSize;
  REQUIRE_OK(fixture.storage->corrupt_volatile(payload_offset, 1));
  REQUIRE_OK(fixture.storage->corrupt_durable(payload_offset, 1));
  fixture.store.reset();

  tfdb::OpenOptions lazy;
  std::unique_ptr<tfdb::RingStore> lazy_store;
  REQUIRE_OK(tfdb::RingStore::open(fixture.storage, lazy, &lazy_store));
  bool saw_corruption = false;
  REQUIRE_OK(lazy_store->scan_blocks(tfdb::QueryOptions(),
      [&](const tfdb::BlockEvent& event) {
        if (event.kind == tfdb::BlockEventKind::corrupt_gap)
          saw_corruption = true;
        return true;
      }));
  REQUIRE(saw_corruption);
  tfdb::QueryOptions stop_on_gap;
  stop_on_gap.continue_on_gap = false;
  REQUIRE(lazy_store->scan_blocks(stop_on_gap,
      [](const tfdb::BlockEvent&) { return true; }).code() ==
      tfdb::StatusCode::corrupt);
  lazy_store.reset();

  tfdb::OpenOptions strict;
  strict.verify_payloads_on_open = true;
  std::unique_ptr<tfdb::RingStore> strict_store;
  REQUIRE(tfdb::RingStore::open(fixture.storage, strict, &strict_store).code() ==
          tfdb::StatusCode::corrupt);

}

TEST(eager_open_verification_rejects_valid_crc_wrong_footer_summary) {
  Fixture fixture;
  fixture.open();
  const char value = 'f';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), 42));
  REQUIRE_OK(fixture.store->checkpoint());
  REQUIRE_OK(fixture.store->rotate(fixture.partition));
  fixture.store.reset();

  const std::uint64_t footer_offset = tfdb::internal::kVolumePrefixSize +
      fixture.volume.partition_size -
      tfdb::internal::kPartitionFooterRegionSize;
  std::vector<std::uint8_t> footer_bytes(
      tfdb::internal::kPartitionFooterEncodedSize);
  REQUIRE_OK(tfdb::read_exact(*fixture.storage, footer_offset,
      tfdb::MutableByteView(footer_bytes.data(), footer_bytes.size())));
  tfdb::internal::PartitionFooter footer;
  REQUIRE_OK(tfdb::internal::decode_partition_footer(
      tfdb::ByteView(footer_bytes), &footer));
  footer.min_time_ns = 41;
  footer_bytes = tfdb::internal::encode_partition_footer(footer);
  REQUIRE_OK(tfdb::write_exact(*fixture.storage, footer_offset,
                               tfdb::ByteView(footer_bytes)));
  REQUIRE_OK(fixture.storage->flush());

  tfdb::OpenOptions strict;
  strict.verify_payloads_on_open = true;
  std::unique_ptr<tfdb::RingStore> strict_store;
  REQUIRE(tfdb::RingStore::open(fixture.storage, strict,
                                &strict_store).code() ==
          tfdb::StatusCode::corrupt);
}

TEST(eager_open_verification_exercises_registered_decoder) {
  Fixture fixture;
  fixture.partition.compression = static_cast<tfdb::CompressionId>(100);
  fixture.partition.compression_version = 7;
  tfdb::OpenOptions writer_options;
  writer_options.writable = true;
  writer_options.next_partition = fixture.partition;
  writer_options.compression_codecs.push_back(
      std::shared_ptr<const tfdb::CompressionCodec>(new RepeatedByteCodec()));
  REQUIRE_OK(tfdb::RingStore::open(fixture.storage, writer_options,
                                   &fixture.store));
  const std::vector<std::uint8_t> repeated(100, 0x5a);
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(repeated), 1));
  REQUIRE_OK(fixture.store->rotate(fixture.partition));
  fixture.store.reset();

  tfdb::OpenOptions lazy;
  lazy.compression_codecs.push_back(
      std::shared_ptr<const tfdb::CompressionCodec>(
          new RejectingRepeatedByteDecoder()));
  std::unique_ptr<tfdb::RingStore> lazy_store;
  REQUIRE_OK(tfdb::RingStore::open(fixture.storage, lazy, &lazy_store));
  lazy_store.reset();

  tfdb::OpenOptions strict = lazy;
  strict.verify_payloads_on_open = true;
  std::unique_ptr<tfdb::RingStore> strict_store;
  REQUIRE(tfdb::RingStore::open(fixture.storage, strict, &strict_store).code() ==
          tfdb::StatusCode::corrupt);

}

TEST(eager_open_verification_decodes_footerless_prefix) {
  Fixture fixture;
  fixture.partition.compression = static_cast<tfdb::CompressionId>(100);
  fixture.partition.compression_version = 7;
  tfdb::OpenOptions writer_options;
  writer_options.writable = true;
  writer_options.next_partition = fixture.partition;
  writer_options.compression_codecs.push_back(
      std::shared_ptr<const tfdb::CompressionCodec>(new RepeatedByteCodec()));
  REQUIRE_OK(tfdb::RingStore::open(fixture.storage, writer_options,
                                   &fixture.store));
  const std::vector<std::uint8_t> repeated(100, 0x33);
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(repeated), 1));
  REQUIRE_OK(fixture.store->checkpoint());
  fixture.store.reset();

  tfdb::OpenOptions strict;
  strict.verify_payloads_on_open = true;
  strict.compression_codecs.push_back(
      std::shared_ptr<const tfdb::CompressionCodec>(
          new RejectingRepeatedByteDecoder()));
  std::unique_ptr<tfdb::RingStore> strict_store;
  REQUIRE(tfdb::RingStore::open(fixture.storage, strict, &strict_store).code() ==
          tfdb::StatusCode::corrupt);
}

TEST(lazy_read_rejects_codec_not_declared_by_partition) {
  Fixture fixture;
  fixture.partition.compression = static_cast<tfdb::CompressionId>(100);
  fixture.partition.compression_version = 7;
  tfdb::OpenOptions writer_options;
  writer_options.writable = true;
  writer_options.next_partition = fixture.partition;
  writer_options.compression_codecs.push_back(
      std::shared_ptr<const tfdb::CompressionCodec>(new RepeatedByteCodec()));
  REQUIRE_OK(tfdb::RingStore::open(fixture.storage, writer_options,
                                   &fixture.store));
  const std::vector<std::uint8_t> repeated(100, 0x44);
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(repeated), 1));
  REQUIRE_OK(fixture.store->rotate(fixture.partition));
  fixture.store.reset();

  std::vector<std::uint8_t> partition_bytes(
      tfdb::internal::kPartitionHeaderEncodedSize);
  REQUIRE_OK(tfdb::read_exact(
      *fixture.storage, tfdb::internal::kVolumePrefixSize,
      tfdb::MutableByteView(partition_bytes.data(), partition_bytes.size())));
  tfdb::internal::PartitionHeader header;
  REQUIRE_OK(tfdb::internal::decode_partition_header(
      tfdb::ByteView(partition_bytes), &header));
  header.options.compression = tfdb::CompressionId::none;
  header.options.compression_version = 1;
  partition_bytes = tfdb::internal::encode_partition_header(
      header, fixture.volume.partition_size, fixture.volume.index_region_size);
  REQUIRE_OK(tfdb::write_exact(*fixture.storage,
      tfdb::internal::kVolumePrefixSize, tfdb::ByteView(partition_bytes)));
  REQUIRE_OK(fixture.storage->flush());

  tfdb::OpenOptions lazy;
  lazy.compression_codecs.push_back(
      std::shared_ptr<const tfdb::CompressionCodec>(new RepeatedByteCodec()));
  std::unique_ptr<tfdb::RingStore> store;
  REQUIRE_OK(tfdb::RingStore::open(fixture.storage, lazy, &store));
  std::size_t corrupt_gaps = 0;
  REQUIRE_OK(store->scan_blocks(tfdb::QueryOptions(),
      [&](const tfdb::BlockEvent& event) {
        if (event.kind == tfdb::BlockEventKind::corrupt_gap) ++corrupt_gaps;
        return true;
      }));
  REQUIRE_EQ(corrupt_gaps, 1u);
}

TEST(one_valid_volume_header_copy_is_sufficient) {
  Fixture fixture;
  REQUIRE_OK(fixture.storage->corrupt_volatile(0, 1));
  REQUIRE_OK(fixture.storage->corrupt_durable(0, 1));
  fixture.open(false);
  tfdb::VolumeInfo info;
  REQUIRE_OK(fixture.store->inspect(&info));
}

TEST(damaged_partition_magic_is_found_from_current_block_evidence) {
  Fixture fixture;
  fixture.open();
  const char value = 'h';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), 1));
  REQUIRE_OK(fixture.store->checkpoint());
  const std::uint64_t header_offset = tfdb::internal::kVolumePrefixSize;
  REQUIRE_OK(fixture.storage->corrupt_volatile(header_offset, 1));
  REQUIRE_OK(fixture.storage->corrupt_durable(header_offset, 1));
  fixture.store.reset();

  tfdb::OpenOptions lazy;
  std::unique_ptr<tfdb::RingStore> lazy_store;
  REQUIRE_OK(tfdb::RingStore::open(fixture.storage, lazy, &lazy_store));
  std::size_t corrupt_gaps = 0;
  REQUIRE_OK(lazy_store->scan_blocks(tfdb::QueryOptions(),
      [&](const tfdb::BlockEvent& event) {
        if (event.kind == tfdb::BlockEventKind::corrupt_gap) ++corrupt_gaps;
        return true;
      }));
  REQUIRE_EQ(corrupt_gaps, 1u);
  lazy_store.reset();

  tfdb::OpenOptions strict;
  strict.verify_payloads_on_open = true;
  std::unique_ptr<tfdb::RingStore> strict_store;
  REQUIRE(tfdb::RingStore::open(fixture.storage, strict, &strict_store).code() ==
          tfdb::StatusCode::corrupt);

  tfdb::OpenOptions writable;
  writable.writable = true;
  std::unique_ptr<tfdb::RingStore> writable_store;
  REQUIRE(tfdb::RingStore::open(fixture.storage, writable,
                                &writable_store).code() ==
          tfdb::StatusCode::corrupt);
}

TEST(valid_disagreeing_volume_headers_are_corruption) {
  Fixture fixture;
  tfdb::internal::VolumeHeader alternate;
  alternate.volume_id_high = fixture.volume.volume_id_high;
  alternate.volume_id_low = fixture.volume.volume_id_low + 1;
  alternate.volume_size = fixture.storage->size();
  alternate.partition_size = fixture.volume.partition_size;
  alternate.partition_count = 3;
  alternate.index_region_size = fixture.volume.index_region_size;
  alternate.max_block_payload = fixture.volume.max_block_payload;
  alternate.persistence_quantum = fixture.volume.persistence_quantum;
  const auto bytes = tfdb::internal::encode_volume_header(alternate);
  REQUIRE_OK(tfdb::write_exact(*fixture.storage, tfdb::internal::kVolumeHeaderCopySize,
                               tfdb::ByteView(bytes)));
  REQUIRE_OK(fixture.storage->flush());
  tfdb::OpenOptions options;
  std::unique_ptr<tfdb::RingStore> store;
  REQUIRE(tfdb::RingStore::open(fixture.storage, options, &store).code() == tfdb::StatusCode::corrupt);
}

TEST(generation_exhaustion_is_explicit_and_preserves_readability) {
  Fixture fixture;
  tfdb::internal::PartitionHeader header;
  header.volume_id_high = fixture.volume.volume_id_high;
  header.volume_id_low = fixture.volume.volume_id_low;
  header.generation = UINT64_MAX;
  header.slot = 0;
  header.max_block_payload = fixture.volume.max_block_payload;
  header.persistence_quantum = fixture.volume.persistence_quantum;
  header.options = fixture.partition;
  const std::vector<std::uint8_t> bytes =
      tfdb::internal::encode_partition_header(
          header, fixture.volume.partition_size,
          fixture.volume.index_region_size);
  REQUIRE_OK(tfdb::write_exact(*fixture.storage,
      tfdb::internal::kVolumePrefixSize, tfdb::ByteView(bytes)));
  REQUIRE_OK(fixture.storage->flush());
  fixture.open();
  REQUIRE(fixture.store->rotate(fixture.partition).code() ==
          tfdb::StatusCode::generation_exhausted);
  fixture.store.reset();
  fixture.open(false);
  tfdb::VolumeInfo info;
  REQUIRE_OK(fixture.store->inspect(&info));
  REQUIRE_EQ(info.partitions.size(), 1u);
  REQUIRE_EQ(info.partitions[0].generation, UINT64_MAX);
}

TEST(hard_write_error_faults_writer_until_reopen) {
  Fixture fixture;
  fixture.open();
  fixture.storage->reset_counters();
  tfdb::FaultRule fault;
  fault.operation = tfdb::StorageOperation::write;
  fault.call = 1;
  fault.result = tfdb::StatusCode::no_space;
  fixture.storage->add_fault(fault);
  const char byte = 'z';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&byte, 1), 1));
  const tfdb::Status original = fixture.store->publish();
  REQUIRE(original.code() == tfdb::StatusCode::no_space);
  REQUIRE(!original.message().empty());
  const auto require_original = [&](const tfdb::Status& status) {
    REQUIRE(status.code() == original.code());
    REQUIRE(status.message() == original.message());
  };
  require_original(
      fixture.store->append(tfdb::ByteView(&byte, 1), 2));
  require_original(fixture.store->writer_status());
  require_original(
      fixture.store->set_next_partition_options(fixture.partition));
  tfdb::AsyncWriterOptions async_options;
  tfdb::AsyncWriter fresh_writer(*fixture.store, async_options);
  require_original(fresh_writer.start());
  fixture.storage->clear_faults();
  fixture.storage->crash_discard_volatile();
  fixture.store.reset();
  fixture.open();
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&byte, 1), 3));
  REQUIRE_OK(fixture.store->checkpoint());
}

TEST(short_write_is_completed_at_remaining_offset) {
  Fixture fixture;
  fixture.open();
  fixture.storage->reset_counters();
  tfdb::FaultRule short_write;
  short_write.operation = tfdb::StorageOperation::write;
  short_write.call = 1;
  short_write.transfer_before_error = 10;
  short_write.result = tfdb::StatusCode::ok;
  fixture.storage->add_fault(short_write);
  const char byte = 's';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&byte, 1), 1));
  REQUIRE_OK(fixture.store->checkpoint());
  REQUIRE_EQ(fixture.storage->counters().write_calls, 2u);
  const std::vector<tfdb::StorageEvent> events = fixture.storage->events();
  std::vector<tfdb::StorageEvent> writes;
  for (const tfdb::StorageEvent& event : events)
    if (event.operation == tfdb::StorageOperation::write) writes.push_back(event);
  REQUIRE_EQ(writes.size(), 2u);
  REQUIRE_EQ(writes[0].transferred, 10u);
  REQUIRE_EQ(writes[1].offset, writes[0].offset + 10u);
  REQUIRE_EQ(writes[1].requested, writes[0].requested - 10u);
  REQUIRE_EQ(collect_blocks(*fixture.store).size(), 1u);
}

TEST(interrupted_and_zero_progress_writes_are_handled) {
  {
    Fixture fixture;
    fixture.open();
    fixture.storage->reset_counters();
    tfdb::FaultRule interrupted;
    interrupted.operation = tfdb::StorageOperation::write;
    interrupted.call = 1;
    interrupted.result = tfdb::StatusCode::interrupted;
    fixture.storage->add_fault(interrupted);
    const char value = 'i';
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), 1));
    REQUIRE_OK(fixture.store->checkpoint());
    REQUIRE_EQ(fixture.storage->counters().write_calls, 2u);
  }
  {
    Fixture fixture;
    fixture.open();
    fixture.storage->reset_counters();
    tfdb::FaultRule no_progress;
    no_progress.operation = tfdb::StorageOperation::write;
    no_progress.call = 1;
    no_progress.result = tfdb::StatusCode::ok;
    no_progress.transfer_before_error = 0;
    fixture.storage->add_fault(no_progress);
    const char value = 'z';
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), 1));
    REQUIRE(fixture.store->publish().code() == tfdb::StatusCode::io_error);
  }
}

TEST(memory_fault_model_epochs_and_materialization_are_atomic) {
  tfdb::MemoryStorage storage(32);
  const std::uint8_t first[] = {1, 2, 3, 4};
  REQUIRE_OK(tfdb::write_exact(storage, 0, tfdb::ByteView(first, sizeof first)));
  storage.reset_counters();
  REQUIRE(storage.crash_materialize({{1, 0, 1}}).code() ==
          tfdb::StatusCode::invalid_argument);
  REQUIRE(storage.durable_image() == std::vector<std::uint8_t>(32, 0));

  const std::uint8_t second[] = {5, 6, 7, 8};
  REQUIRE_OK(tfdb::write_exact(storage, 8, tfdb::ByteView(second, sizeof second)));
  const std::vector<std::uint8_t> durable_before = storage.durable_image();
  REQUIRE(storage.crash_materialize({{1, 0, 2}, {99, 0, 1}}).code() ==
          tfdb::StatusCode::invalid_argument);
  REQUIRE(storage.durable_image() == durable_before);
}

TEST(memory_fault_rules_remain_installed_across_counter_epochs) {
  tfdb::MemoryStorage storage(16);
  storage.add_fault({tfdb::StorageOperation::write, 1, 0,
                     tfdb::StatusCode::io_error});
  const std::uint8_t value = 7;
  REQUIRE(tfdb::write_exact(storage, 0, tfdb::ByteView(&value, 1)).code() ==
          tfdb::StatusCode::io_error);
  storage.reset_counters();
  REQUIRE(tfdb::write_exact(storage, 0, tfdb::ByteView(&value, 1)).code() ==
          tfdb::StatusCode::io_error);
  storage.clear_faults();
  storage.reset_counters();
  REQUIRE_OK(tfdb::write_exact(storage, 0, tfdb::ByteView(&value, 1)));
}

TEST(exact_reads_handle_short_interrupted_zero_and_hard_errors) {
  tfdb::MemoryStorage storage(16);
  const std::uint8_t input[] = {1, 2, 3, 4};
  REQUIRE_OK(tfdb::write_exact(storage, 0, tfdb::ByteView(input, sizeof input)));
  REQUIRE_OK(storage.flush());
  std::uint8_t output[4] = {};

  storage.reset_counters();
  storage.add_fault({tfdb::StorageOperation::read, 1, 0,
                     tfdb::StatusCode::interrupted});
  REQUIRE_OK(tfdb::read_exact(storage, 0,
                              tfdb::MutableByteView(output, sizeof output)));
  REQUIRE_EQ(storage.counters().read_calls, 2u);
  REQUIRE(std::equal(output, output + sizeof output, input));

  storage.clear_faults();
  storage.reset_counters();
  storage.add_fault({tfdb::StorageOperation::read, 1, 2,
                     tfdb::StatusCode::ok});
  REQUIRE_OK(tfdb::read_exact(storage, 0,
                              tfdb::MutableByteView(output, sizeof output)));
  REQUIRE_EQ(storage.counters().read_calls, 2u);

  storage.clear_faults();
  storage.reset_counters();
  storage.add_fault({tfdb::StorageOperation::read, 1, 0,
                     tfdb::StatusCode::ok});
  REQUIRE(tfdb::read_exact(storage, 0,
      tfdb::MutableByteView(output, sizeof output)).code() ==
      tfdb::StatusCode::io_error);

  storage.clear_faults();
  storage.reset_counters();
  storage.add_fault({tfdb::StorageOperation::read, 1, 0,
                     tfdb::StatusCode::io_error});
  REQUIRE(tfdb::read_exact(storage, 0,
      tfdb::MutableByteView(output, sizeof output)).code() ==
      tfdb::StatusCode::io_error);
}

TEST(flush_failure_never_advances_durable_metrics) {
  Fixture fixture;
  fixture.open();
  const std::uint64_t sync_calls_before = fixture.store->metrics().sync_calls;
  fixture.storage->reset_counters();
  tfdb::FaultRule fault;
  fault.operation = tfdb::StorageOperation::flush;
  fault.call = 1;
  fault.result = tfdb::StatusCode::io_error;
  fixture.storage->add_fault(fault);
  const char value = 'd';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), 1));
  REQUIRE(fixture.store->checkpoint().code() == tfdb::StatusCode::io_error);
  const tfdb::StoreMetrics metrics = fixture.store->metrics();
  REQUIRE_EQ(metrics.durable_records, 0u);
  REQUIRE_EQ(metrics.durable_blocks, 0u);
  REQUIRE_EQ(metrics.sync_errors, 1u);
  REQUIRE_EQ(metrics.sync_calls, sync_calls_before + 1u);
}

TEST(rotation_commit_failpoints_preserve_last_checkpoint) {
  for (tfdb::StorageOperation operation :
       {tfdb::StorageOperation::write, tfdb::StorageOperation::flush}) {
    const std::uint64_t last_call = operation == tfdb::StorageOperation::write ? 3 : 2;
    for (std::uint64_t call = 1; call <= last_call; ++call) {
      Fixture fixture;
      fixture.open();
      const char value = 'c';
      REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), 1));
      REQUIRE_OK(fixture.store->checkpoint());
      fixture.storage->reset_counters();
      tfdb::FaultRule fault;
      fault.operation = operation;
      fault.call = call;
      fault.result = tfdb::StatusCode::io_error;
      fixture.storage->add_fault(fault);
      const tfdb::Status rotation = fixture.store->rotate(fixture.partition);
      REQUIRE(rotation.code() == tfdb::StatusCode::io_error);
      fixture.storage->crash_discard_volatile();
      fixture.storage->clear_faults();
      fixture.store.reset();
      fixture.open(false);
      const auto blocks = collect_blocks(*fixture.store);
      REQUIRE_EQ(blocks.size(), 1u);
      REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('c'));
      fixture.store.reset();
      fixture.open();
      const char continued = 'n';
      REQUIRE_OK(fixture.store->append(tfdb::ByteView(&continued, 1), 2));
      REQUIRE_OK(fixture.store->checkpoint());
    }
  }
}

TEST(dirty_rotation_failpoints_have_explicit_recovery_oracle) {
  for (tfdb::StorageOperation operation :
       {tfdb::StorageOperation::write, tfdb::StorageOperation::flush}) {
    const std::uint64_t last_call =
        operation == tfdb::StorageOperation::write ? 4 : 3;
    for (std::uint64_t call = 1; call <= last_call; ++call) {
      Fixture fixture;
      fixture.open();
      const char baseline = 'A', pending = 'B';
      REQUIRE_OK(fixture.store->append(tfdb::ByteView(&baseline, 1), 1));
      REQUIRE_OK(fixture.store->checkpoint());
      REQUIRE_OK(fixture.store->append(tfdb::ByteView(&pending, 1), 2));
      fixture.storage->reset_counters();
      fixture.storage->add_fault(
          {operation, call, 0, tfdb::StatusCode::io_error});
      REQUIRE(fixture.store->rotate(fixture.partition).code() ==
              tfdb::StatusCode::io_error);
      fixture.storage->crash_discard_volatile();
      fixture.storage->clear_faults();
      fixture.store.reset();
      fixture.open(false);
      const auto blocks = collect_blocks(*fixture.store);
      const std::size_t expected =
          (operation == tfdb::StorageOperation::write && call == 1) ||
          (operation == tfdb::StorageOperation::flush && call == 1) ? 1u : 2u;
      REQUIRE_EQ(blocks.size(), expected);
      REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('A'));
      if (expected == 2)
        REQUIRE_EQ(blocks[1][0], static_cast<std::uint8_t>('B'));

      fixture.store.reset();
      fixture.open();
      const char after = 'C';
      REQUIRE_OK(fixture.store->append(tfdb::ByteView(&after, 1), 3));
      REQUIRE_OK(fixture.store->checkpoint());
    }
  }
}

TEST(valid_footer_persisted_without_index_falls_back_to_block_scan) {
  Fixture fixture;
  fixture.open();
  const char value = 'r';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), 1));
  REQUIRE_OK(fixture.store->checkpoint());
  fixture.storage->reset_counters();
  tfdb::FaultRule failed_seal_flush;
  failed_seal_flush.operation = tfdb::StorageOperation::flush;
  failed_seal_flush.call = 1;
  failed_seal_flush.result = tfdb::StatusCode::io_error;
  fixture.storage->add_fault(failed_seal_flush);
  REQUIRE(fixture.store->rotate(fixture.partition).code() ==
          tfdb::StatusCode::io_error);
  // Write call 1 is the index and call 2 is the footer. Model a controller
  // that persisted the later footer write but not the earlier index write.
  REQUIRE_OK(fixture.storage->crash_materialize({
      {2, 0, tfdb::internal::kPartitionFooterEncodedSize}}));
  fixture.storage->clear_faults();
  fixture.store.reset();
  fixture.open(false);
  const auto blocks = collect_blocks_with_gaps(*fixture.store, 1);
  REQUIRE_EQ(blocks.size(), 1u);
  REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('r'));
}

TEST(seal_index_footer_subsets_have_explicit_recovery_oracle) {
  enum class Persisted { none, index_prefix, index_only, footer_prefix,
                         footer_only, footer_with_torn_index,
                         footer_then_index, index_then_footer };
  for (Persisted persisted :
       {Persisted::none, Persisted::index_prefix, Persisted::index_only,
        Persisted::footer_prefix, Persisted::footer_only,
        Persisted::footer_with_torn_index, Persisted::footer_then_index,
        Persisted::index_then_footer}) {
    Fixture fixture;
    fixture.open();
    const char a = 'A';
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(&a, 1), 1));
    REQUIRE_OK(fixture.store->checkpoint());
    fixture.storage->reset_counters();
    fixture.storage->add_fault({tfdb::StorageOperation::flush, 1, 0,
                                tfdb::StatusCode::io_error});
    REQUIRE(fixture.store->rotate(fixture.partition).code() ==
            tfdb::StatusCode::io_error);
    const std::vector<tfdb::StorageEvent> events = fixture.storage->events();
    REQUIRE_EQ(events.size(), 3u);
    REQUIRE(events[0].operation == tfdb::StorageOperation::write);
    REQUIRE(events[1].operation == tfdb::StorageOperation::write);
    REQUIRE(events[2].operation == tfdb::StorageOperation::flush);
    REQUIRE_EQ(events[0].requested, tfdb::internal::kIndexEntryEncodedSize);
    REQUIRE_EQ(events[1].requested,
               tfdb::internal::kPartitionFooterEncodedSize);
    std::vector<tfdb::WriteFragment> fragments;
    if (persisted == Persisted::index_prefix)
      fragments.push_back({1, 0, events[0].requested - 1});
    else if (persisted == Persisted::index_only)
      fragments.push_back({1, 0, events[0].requested});
    else if (persisted == Persisted::footer_prefix)
      fragments.push_back({2, 0, 64});
    else if (persisted == Persisted::footer_only)
      fragments.push_back({2, 0, events[1].requested});
    else if (persisted == Persisted::footer_with_torn_index) {
      fragments.push_back({2, 0, events[1].requested});
      fragments.push_back({1, 0, 20});
    }
    else if (persisted == Persisted::footer_then_index) {
      fragments.push_back({2, 0, events[1].requested});
      fragments.push_back({1, 0, events[0].requested});
    } else if (persisted == Persisted::index_then_footer) {
      fragments.push_back({1, 0, events[0].requested});
      fragments.push_back({2, 0, events[1].requested});
    }
    REQUIRE_OK(fixture.storage->crash_materialize(fragments));
    fixture.storage->clear_faults();
    fixture.store.reset();
    fixture.open(false);
    std::size_t gaps = 0;
    const auto blocks = [&] {
      std::vector<std::vector<std::uint8_t>> result;
      REQUIRE_OK(fixture.store->scan_blocks(tfdb::QueryOptions(),
          [&](const tfdb::BlockEvent& event) {
            if (event.kind == tfdb::BlockEventKind::data)
              result.emplace_back(event.data.data(),
                                  event.data.data() + event.data.size());
            else
              ++gaps;
            return true;
          }));
      return result;
    }();
    REQUIRE_EQ(blocks.size(), 1u);
    REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('A'));
    const bool invalid_seal = persisted == Persisted::footer_only ||
                              persisted == Persisted::footer_with_torn_index;
    REQUIRE_EQ(gaps, invalid_seal ? 1u : 0u);

    fixture.store.reset();
    fixture.open();
    const char c = 'C';
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(&c, 1), 3));
    REQUIRE_OK(fixture.store->checkpoint());
    std::size_t continued_gaps = 0;
    std::vector<std::vector<std::uint8_t>> continued;
    REQUIRE_OK(fixture.store->scan_blocks(tfdb::QueryOptions(),
        [&](const tfdb::BlockEvent& event) {
          if (event.kind == tfdb::BlockEventKind::data)
            continued.emplace_back(event.data.data(),
                                   event.data.data() + event.data.size());
          else
            ++continued_gaps;
          return true;
        }));
    REQUIRE_EQ(continued.size(), 2u);
    REQUIRE_EQ(continued[0][0], static_cast<std::uint8_t>('A'));
    REQUIRE_EQ(continued[1][0], static_cast<std::uint8_t>('C'));
    REQUIRE_EQ(continued_gaps, invalid_seal ? 1u : 0u);
  }
}

TEST(new_partition_header_torn_boundaries_are_safe_or_explicit) {
  const std::size_t cuts[] = {0, 1, 7, 8, 16, 31, 32, 39, 40, 103,
                              104, 107, 108, 127, 128, 167, 168, 207,
                              208, 247, 248, 256, 257, 258, 511, 512,
                              1023, 1024};
  for (std::size_t cut : cuts) {
    Fixture fixture;
    fixture.open();
    const char a = 'A';
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(&a, 1), 1));
    REQUIRE_OK(fixture.store->checkpoint());
    fixture.storage->reset_counters();
    fixture.storage->add_fault({tfdb::StorageOperation::flush, 2, 0,
                                tfdb::StatusCode::io_error});
    REQUIRE(fixture.store->rotate(fixture.partition).code() ==
            tfdb::StatusCode::io_error);
    const std::vector<tfdb::StorageEvent> events = fixture.storage->events();
    REQUIRE_EQ(events.size(), 5u);
    REQUIRE(events[3].operation == tfdb::StorageOperation::write);
    REQUIRE_EQ(events[3].requested,
               tfdb::internal::kPartitionHeaderEncodedSize);
    std::vector<tfdb::WriteFragment> fragments;
    if (cut != 0) fragments.push_back({3, 0, cut});
    REQUIRE_OK(fixture.storage->crash_materialize(fragments));
    fixture.storage->clear_faults();
    fixture.store.reset();
    fixture.open(false);
    std::size_t gaps = 0;
    std::size_t data = 0;
    REQUIRE_OK(fixture.store->scan_blocks(tfdb::QueryOptions(),
        [&](const tfdb::BlockEvent& event) {
          if (event.kind == tfdb::BlockEventKind::data) ++data;
          else ++gaps;
          return true;
        }));
    REQUIRE_EQ(data, 1u);
    // Prefix 32 proves the UUID; prefix 257 includes the last nonzero byte of
    // this default header. The unwritten suffix is canonical reserved zero.
    const bool identified_damage = cut >= 32 && cut < 257;
    REQUIRE_EQ(gaps, identified_damage ? 1u : 0u);

    fixture.store.reset();
    tfdb::OpenOptions writable;
    writable.writable = true;
    writable.next_partition = fixture.partition;
    const tfdb::Status reopened = tfdb::RingStore::open(
        fixture.storage, writable, &fixture.store);
    if (identified_damage) {
      REQUIRE(reopened.code() == tfdb::StatusCode::corrupt);
    } else {
      REQUIRE_OK(reopened);
      const char b = 'B';
      REQUIRE_OK(fixture.store->append(tfdb::ByteView(&b, 1), 2));
      REQUIRE_OK(fixture.store->checkpoint());
    }
  }
}

TEST(reused_partition_header_torn_boundaries_do_not_resurrect_old_slot) {
  const std::size_t cuts[] = {0, 31, 32, 33, 39, 40, 103, 104,
                              107, 108, 1024};
  for (std::size_t cut : cuts) {
    Fixture fixture(64u * 1024u, 256, 512, 2);
    fixture.open();
    const char a = 'A', b = 'B';
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(&a, 1), 1));
    REQUIRE_OK(fixture.store->checkpoint());
    REQUIRE_OK(fixture.store->rotate(fixture.partition));
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(&b, 1), 2));
    REQUIRE_OK(fixture.store->checkpoint());
    fixture.storage->reset_counters();
    fixture.storage->add_fault({tfdb::StorageOperation::flush, 2, 0,
                                tfdb::StatusCode::io_error});
    REQUIRE(fixture.store->rotate(fixture.partition).code() ==
            tfdb::StatusCode::io_error);
    const std::vector<tfdb::StorageEvent> events = fixture.storage->events();
    REQUIRE_EQ(events.size(), 5u);
    REQUIRE(events[3].operation == tfdb::StorageOperation::write);
    std::vector<tfdb::WriteFragment> fragments;
    if (cut != 0) fragments.push_back({3, 0, cut});
    REQUIRE_OK(fixture.storage->crash_materialize(fragments));
    fixture.storage->clear_faults();
    fixture.store.reset();
    fixture.open(false);
    std::size_t gaps = 0;
    std::vector<std::uint8_t> values;
    REQUIRE_OK(fixture.store->scan_blocks(tfdb::QueryOptions(),
        [&](const tfdb::BlockEvent& event) {
          if (event.kind == tfdb::BlockEventKind::data)
            values.push_back(event.data.data()[0]);
          else
            ++gaps;
          return true;
        }));
    const bool old_header_unchanged = cut <= 32;
    const bool identified_damage = cut >= 33 && cut < 108;
    REQUIRE_EQ(gaps, identified_damage ? 1u : 0u);
    if (old_header_unchanged) {
      REQUIRE(values == std::vector<std::uint8_t>({'A', 'B'}));
    } else {
      REQUIRE(values == std::vector<std::uint8_t>({'B'}));
    }
    fixture.store.reset();
    tfdb::OpenOptions writable;
    writable.writable = true;
    writable.next_partition = fixture.partition;
    const tfdb::Status reopened = tfdb::RingStore::open(
        fixture.storage, writable, &fixture.store);
    if (identified_damage) {
      REQUIRE(reopened.code() == tfdb::StatusCode::corrupt);
    } else {
      REQUIRE_OK(reopened);
      const char c = 'C';
      REQUIRE_OK(fixture.store->append(tfdb::ByteView(&c, 1), 3));
      REQUIRE_OK(fixture.store->checkpoint());
      const auto current = collect_blocks(*fixture.store);
      REQUIRE_EQ(current.back()[0], static_cast<std::uint8_t>('C'));
    }
  }
}

TEST(framed_record_crc_covers_metadata_and_signed_time) {
  std::vector<std::uint8_t> encoded;
  const char payload[] = {'l', 'o', 'g'};
  REQUIRE_OK(tfdb::FramedRecordV1::encode(
      std::numeric_limits<std::int64_t>::min(), 0x1122334455667788ull,
      7, tfdb::ByteView(payload, sizeof payload), &encoded));
  tfdb::FramedRecordV1 profile;
  std::int64_t decoded_time = 0;
  REQUIRE_OK(profile.decode_block(tfdb::ByteView(encoded),
      [&](const tfdb::RecordView& record) {
        decoded_time = record.index_time_ns;
        REQUIRE_EQ(record.selector, 0x1122334455667788ull);
        REQUIRE_EQ(record.flags, 7u);
        return true;
      }));
  REQUIRE_EQ(decoded_time, std::numeric_limits<std::int64_t>::min());
  std::vector<std::uint8_t> with_trailing_byte = encoded;
  with_trailing_byte.push_back(0);
  REQUIRE(profile.decode_block(tfdb::ByteView(with_trailing_byte),
      [](const tfdb::RecordView&) { return true; }).code() ==
      tfdb::StatusCode::corrupt);
  encoded[8] ^= 1;
  REQUIRE(profile.decode_block(tfdb::ByteView(encoded),
      [](const tfdb::RecordView&) { return true; }).code() ==
      tfdb::StatusCode::corrupt);
}

TEST(time_domains_are_never_compared_implicitly) {
  Fixture fixture;
  fixture.partition.time_domain_id = 2;
  fixture.open();
  const char value = 't';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), 10));
  REQUIRE_OK(fixture.store->checkpoint());
  std::size_t count = 0;
  REQUIRE_OK(fixture.store->query_blocks({0, 20, 1}, tfdb::QueryOptions(),
      [&](const tfdb::BlockEvent& event) {
        if (event.kind == tfdb::BlockEventKind::data) ++count;
        return true;
      }));
  REQUIRE_EQ(count, 0u);
  REQUIRE_OK(fixture.store->query_blocks({0, 20, 2}, tfdb::QueryOptions(),
      [&](const tfdb::BlockEvent& event) {
        if (event.kind == tfdb::BlockEventKind::data) ++count;
        return true;
      }));
  REQUIRE_EQ(count, 1u);
  REQUIRE(fixture.store->query_blocks({0, 20, 0}, tfdb::QueryOptions(),
      [](const tfdb::BlockEvent&) { return true; }).code() ==
      tfdb::StatusCode::invalid_argument);
}

TEST(partition_configuration_changes_only_on_rotation) {
  Fixture fixture;
  fixture.open();
  tfdb::PartitionOptions next = fixture.partition;
  next.compression = tfdb::CompressionId::packbits;
  REQUIRE_OK(fixture.store->set_next_partition_options(next));
  tfdb::PartitionInfo active;
  REQUIRE_OK(fixture.store->active_partition_info(&active));
  REQUIRE(active.options.compression == tfdb::CompressionId::none);
  REQUIRE_OK(fixture.store->rotate(next));
  REQUIRE_OK(fixture.store->active_partition_info(&active));
  REQUIRE(active.options.compression == tfdb::CompressionId::packbits);
  fixture.store.reset();
  fixture.open(false);
  REQUIRE(fixture.store->set_next_partition_options(next).code() ==
          tfdb::StatusCode::io_error);
  REQUIRE(fixture.store->writer_status().code() ==
          tfdb::StatusCode::invalid_argument);
}

TEST(typed_append_rejects_profile_change_during_automatic_rotation) {
  Fixture fixture(16u * 1024u, 900, 1024, 2);
  fixture.volume.index_region_size = 1024;
  REQUIRE_OK(tfdb::RingStore::format(*fixture.storage, fixture.volume));
  fixture.partition.record_format_id = tfdb::kFramedRecordV1ProfileId;
  fixture.partition.record_format_version = 1;
  fixture.open();
  tfdb::PartitionOptions opaque = fixture.partition;
  opaque.record_format_id = 0;
  opaque.record_format_version = 0;
  REQUIRE_OK(fixture.store->set_next_partition_options(opaque));
  const char payload = 'x';
  for (std::int64_t i = 0; i != 6; ++i) {
    REQUIRE_OK(tfdb::append_framed_record(
        *fixture.store, i, static_cast<std::uint64_t>(i), 0,
        tfdb::ByteView(&payload, 1)));
    REQUIRE_OK(fixture.store->checkpoint());
  }
  REQUIRE(tfdb::append_framed_record(
      *fixture.store, 7, 7, 0, tfdb::ByteView(&payload, 1)).code() ==
      tfdb::StatusCode::invalid_argument);
  tfdb::PartitionInfo active;
  REQUIRE_OK(fixture.store->active_partition_info(&active));
  REQUIRE_EQ(active.options.record_format_id, 0u);
  REQUIRE_EQ(active.block_count, 0u);
}

TEST(time_anomaly_comparison_resets_when_domain_changes) {
  Fixture fixture;
  fixture.partition.allowed_backward_skew_ns = 0;
  fixture.partition.allowed_forward_step_ns = 0;
  fixture.open();
  const char value = 't';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), 100));
  REQUIRE_OK(fixture.store->checkpoint());
  tfdb::PartitionOptions next = fixture.partition;
  next.time_domain_id = 2;
  REQUIRE_OK(fixture.store->rotate(next));
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), -100));
  REQUIRE_OK(fixture.store->checkpoint());
  tfdb::PartitionInfo active;
  REQUIRE_OK(fixture.store->active_partition_info(&active));
  REQUIRE((active.flags & tfdb::kBlockFlagTimeAnomaly) == 0);
}

TEST(log_payloads_use_the_same_exact_record_query_path) {
  Fixture fixture;
  fixture.partition.record_format_id = tfdb::kFramedRecordV1ProfileId;
  fixture.partition.record_format_version = 1;
  fixture.open();
  const std::string first = "service=drive severity=warn temperature high";
  const std::string second = "service=network severity=info link restored";
  REQUIRE_OK(tfdb::append_framed_record(*fixture.store, 100, 17, 0,
                                       tfdb::ByteView(first.data(), first.size())));
  REQUIRE_OK(tfdb::append_framed_record(*fixture.store, 101, 23, 0,
                                       tfdb::ByteView(second.data(), second.size())));
  REQUIRE_OK(fixture.store->checkpoint());
  const std::vector<std::uint64_t> selectors =
      collect_selectors(*fixture.store, 0, 200, {23});
  REQUIRE(selectors == std::vector<std::uint64_t>({23}));
}

TEST(reader_reports_overwrite_when_rotation_reuses_snapshot_slot) {
  Fixture fixture(16u * 1024u, 900, 1024, 2);
  fixture.volume.index_region_size = 1024;
  REQUIRE_OK(tfdb::RingStore::format(*fixture.storage, fixture.volume));
  fixture.open();
  std::vector<std::uint8_t> payload(800, 'r');
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(payload), 1));
  REQUIRE_OK(fixture.store->checkpoint());
  std::atomic<bool> triggered(false);
  const std::uint64_t first_payload_offset =
      tfdb::internal::kVolumePrefixSize +
      tfdb::internal::kPartitionHeaderRegionSize +
      tfdb::internal::kBlockHeaderEncodedSize;
  fixture.storage->set_hook([&](tfdb::StorageOperation operation,
                                tfdb::HookPoint point, std::uint64_t,
                                std::uint64_t offset,
                                std::size_t) {
    if (operation != tfdb::StorageOperation::read ||
        point != tfdb::HookPoint::after || offset != first_payload_offset ||
        triggered.exchange(true)) return;
    fixture.storage->set_hook(tfdb::StorageHook());
    REQUIRE_OK(fixture.store->rotate(fixture.partition));
    REQUIRE_OK(fixture.store->rotate(fixture.partition));
    std::vector<std::uint8_t> newer(800, 'n');
    REQUIRE_OK(fixture.store->append(tfdb::ByteView(newer), 2));
    REQUIRE_OK(fixture.store->checkpoint());
  });
  std::size_t overwrite_gaps = 0, data_events = 0;
  std::uint32_t overwritten_slot = UINT32_MAX;
  std::uint64_t overwritten_generation = 0;
  REQUIRE_OK(fixture.store->scan_blocks(tfdb::QueryOptions(),
      [&](const tfdb::BlockEvent& event) {
        if (event.kind == tfdb::BlockEventKind::overwritten_gap) {
          ++overwrite_gaps;
          overwritten_slot = event.metadata.partition_slot;
          overwritten_generation = event.metadata.partition_generation;
        }
        if (event.kind == tfdb::BlockEventKind::data) ++data_events;
        return true;
      }));
  REQUIRE(triggered.load());
  REQUIRE_EQ(overwrite_gaps, 1u);
  REQUIRE_EQ(data_events, 0u);
  REQUIRE_EQ(overwritten_slot, 0u);
  REQUIRE_EQ(overwritten_generation, 1u);

  triggered.store(false);
  fixture.storage->set_hook([&](tfdb::StorageOperation operation,
                                tfdb::HookPoint point, std::uint64_t,
                                std::uint64_t, std::size_t) {
    if (operation != tfdb::StorageOperation::read ||
        point != tfdb::HookPoint::before || triggered.exchange(true)) return;
    fixture.storage->set_hook(tfdb::StorageHook());
    REQUIRE_OK(fixture.store->rotate(fixture.partition));
    REQUIRE_OK(fixture.store->rotate(fixture.partition));
  });
  tfdb::QueryOptions stop_on_gap;
  stop_on_gap.continue_on_gap = false;
  overwrite_gaps = 0;
  const tfdb::Status stopped = fixture.store->scan_blocks(
      stop_on_gap, [&](const tfdb::BlockEvent& event) {
        if (event.kind == tfdb::BlockEventKind::overwritten_gap)
          ++overwrite_gaps;
        return true;
      });
  REQUIRE(stopped.code() == tfdb::StatusCode::overwritten);
  REQUIRE_EQ(overwrite_gaps, 1u);
}

TEST(reader_reports_unchanged_generation_header_damage_as_corruption) {
  Fixture fixture;
  fixture.open();
  const char payload = 'c';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&payload, 1), 1));
  REQUIRE_OK(fixture.store->checkpoint());
  const std::uint64_t block_header_offset =
      tfdb::internal::kVolumePrefixSize +
      tfdb::internal::kPartitionHeaderRegionSize;
  std::atomic<bool> triggered(false);
  fixture.storage->set_hook([&](tfdb::StorageOperation operation,
                                tfdb::HookPoint point, std::uint64_t,
                                std::uint64_t offset, std::size_t) {
    if (operation != tfdb::StorageOperation::read ||
        point != tfdb::HookPoint::before || offset != block_header_offset ||
        triggered.exchange(true)) return;
    fixture.storage->set_hook(tfdb::StorageHook());
    REQUIRE_OK(fixture.storage->corrupt_volatile(
        tfdb::internal::kVolumePrefixSize, 1));
  });
  std::size_t corrupt_gaps = 0, overwritten_gaps = 0, data_events = 0;
  REQUIRE_OK(fixture.store->scan_blocks(tfdb::QueryOptions(),
      [&](const tfdb::BlockEvent& event) {
        if (event.kind == tfdb::BlockEventKind::corrupt_gap) ++corrupt_gaps;
        if (event.kind == tfdb::BlockEventKind::overwritten_gap)
          ++overwritten_gaps;
        if (event.kind == tfdb::BlockEventKind::data) ++data_events;
        return true;
      }));
  REQUIRE(triggered.load());
  REQUIRE_EQ(corrupt_gaps, 1u);
  REQUIRE_EQ(overwritten_gaps, 0u);
  REQUIRE_EQ(data_events, 0u);
}

TEST(multiple_readers_remain_safe_during_hot_rotation) {
  Fixture fixture(16u * 1024u, 900, 1024, 2);
  fixture.volume.index_region_size = 1024;
  REQUIRE_OK(tfdb::RingStore::format(*fixture.storage, fixture.volume));
  fixture.open();
  std::vector<std::uint8_t> initial(800, 0);
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(initial), 0));
  REQUIRE_OK(fixture.store->checkpoint());

  std::atomic<bool> stop(false), failed(false);
  std::atomic<std::uint64_t> data_events(0), gap_events(0);
  std::atomic<std::uint64_t> unexpected_gap_events(0), status_errors(0);
  std::atomic<std::uint64_t> bad_sizes(0), mixed_payloads(0);
  std::mutex failure_detail_mutex;
  std::string first_failure_detail;
  std::vector<std::thread> readers;
  for (unsigned reader = 0; reader != 4; ++reader) {
    readers.emplace_back([&] {
      while (!stop.load()) {
        const tfdb::Status status = fixture.store->scan_blocks(
            tfdb::QueryOptions(), [&](const tfdb::BlockEvent& event) {
              if (event.kind == tfdb::BlockEventKind::data) {
                ++data_events;
                if (event.data.size() != 800) {
                  ++bad_sizes;
                  failed.store(true);
                }
                if (!event.data.empty()) {
                  const std::uint8_t expected = event.data.data()[0];
                  for (std::size_t i = 1; i != event.data.size(); ++i)
                    if (event.data.data()[i] != expected) {
                      ++mixed_payloads;
                      failed.store(true);
                      break;
                    }
                }
              } else {
                ++gap_events;
                if (event.kind != tfdb::BlockEventKind::overwritten_gap) {
                  ++unexpected_gap_events;
                  std::lock_guard<std::mutex> lock(failure_detail_mutex);
                  if (first_failure_detail.empty())
                    first_failure_detail = std::string("unexpected gap: ") +
                        tfdb::status_code_name(event.detail.code()) + ": " +
                        event.detail.message();
                  failed.store(true);
                }
              }
              return true;
            });
        if (!status.ok()) {
          ++status_errors;
          std::lock_guard<std::mutex> lock(failure_detail_mutex);
          if (first_failure_detail.empty())
            first_failure_detail = std::string("query status: ") +
                tfdb::status_code_name(status.code()) + ": " + status.message();
          failed.store(true);
        }
      }
    });
  }
  for (std::uint16_t ordinal = 1; ordinal <= 100; ++ordinal) {
    std::vector<std::uint8_t> payload(800,
        static_cast<std::uint8_t>(ordinal));
    if (!fixture.store->append(tfdb::ByteView(payload), ordinal).ok() ||
        !fixture.store->checkpoint().ok()) {
      failed.store(true);
      break;
    }
  }
  stop.store(true);
  for (std::thread& reader : readers) reader.join();
  if (unexpected_gap_events.load() != 0 || status_errors.load() != 0)
    throw Failure{first_failure_detail};
  REQUIRE_EQ(status_errors.load(), 0u);
  REQUIRE_EQ(bad_sizes.load(), 0u);
  REQUIRE_EQ(mixed_payloads.load(), 0u);
  REQUIRE(!failed.load());
  REQUIRE(data_events.load() != 0);
  REQUIRE(gap_events.load() <= data_events.load() + 400u);
}

TEST(async_checkpoint_is_a_durable_fifo_barrier) {
  Fixture fixture;
  fixture.open();
  tfdb::AsyncWriterOptions options;
  options.queue_capacity_records = 16;
  options.queue_capacity_bytes = 1024;
  options.checkpoint_interval = std::chrono::milliseconds(20);
  tfdb::AsyncWriter writer(*fixture.store, options);
  fixture.storage->reset_counters();
  REQUIRE_OK(writer.start());
  for (char value : {'a', 'b', 'c'})
    REQUIRE_OK(writer.submit(tfdb::ByteView(&value, 1), value));
  REQUIRE_OK(writer.checkpoint());
  REQUIRE(fixture.storage->counters().flush_calls >= 1);
  REQUIRE_EQ(fixture.store->metrics().durable_records, 3u);
  const tfdb::AsyncWriterMetrics async_metrics = writer.metrics();
  REQUIRE_EQ(async_metrics.submitted_records, 3u);
  REQUIRE_EQ(async_metrics.processed_records, 3u);
  REQUIRE_EQ(async_metrics.durable_sequence, 3u);
  REQUIRE_EQ(async_metrics.oldest_undurable_age_ns, 0u);
  REQUIRE_OK(writer.stop());
  fixture.storage->crash_discard_volatile();
  fixture.store.reset();
  fixture.open(false);
  const auto blocks = collect_blocks(*fixture.store);
  REQUIRE_EQ(blocks.size(), 1u);
  REQUIRE_EQ(blocks[0].size(), 3u);
  REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('a'));
  REQUIRE_EQ(blocks[0][1], static_cast<std::uint8_t>('b'));
  REQUIRE_EQ(blocks[0][2], static_cast<std::uint8_t>('c'));
}

TEST(async_checked_submit_revalidates_partition_contract) {
  Fixture fixture;
  fixture.partition.record_format_id = tfdb::kFramedRecordV1ProfileId;
  fixture.partition.record_format_version = 1;
  fixture.open();
  tfdb::AsyncWriterOptions options;
  options.queue_capacity_records = 4;
  options.queue_capacity_bytes = 1024;
  options.checkpoint_interval = std::chrono::milliseconds(20);
  tfdb::AsyncWriter writer(*fixture.store, options);
  tfdb::AppendContract contract;
  contract.record_format_id = tfdb::kFramedRecordV1ProfileId;
  contract.record_format_version = 1;
  contract.time_domain_id = 1;
  const char valid = 'v';
  REQUIRE_OK(writer.start());
  REQUIRE_OK(writer.submit_checked(tfdb::ByteView(&valid, 1), 1, contract));
  REQUIRE_OK(writer.checkpoint());
  REQUIRE_OK(writer.stop());

  REQUIRE_OK(fixture.store->rotate(tfdb::PartitionOptions()));
  const char mislabeled = 'x';
  REQUIRE_OK(writer.start());
  REQUIRE_OK(writer.submit_checked(tfdb::ByteView(&mislabeled, 1), 2,
                                   contract));
  const tfdb::Status rejected = writer.checkpoint();
  REQUIRE(rejected.code() == tfdb::StatusCode::invalid_argument);
  REQUIRE(writer.stop().code() == tfdb::StatusCode::invalid_argument);
  REQUIRE_EQ(fixture.store->metrics().accepted_records, 1u);
}

TEST(async_stop_drains_queue_and_clean_writer_can_restart) {
  Fixture fixture;
  fixture.open();
  tfdb::AsyncWriterOptions options;
  options.queue_capacity_records = 256;
  options.queue_capacity_bytes = 4096;
  options.checkpoint_interval = std::chrono::seconds(10);
  tfdb::AsyncWriter writer(*fixture.store, options);
  REQUIRE_OK(writer.start());
  for (std::uint16_t i = 0; i != 100; ++i) {
    const std::uint8_t value = static_cast<std::uint8_t>(i);
    tfdb::Status submit_status;
    do {
      submit_status = writer.submit(tfdb::ByteView(&value, 1), i);
      if (submit_status.code() == tfdb::StatusCode::busy)
        std::this_thread::yield();
    } while (submit_status.code() == tfdb::StatusCode::busy);
    REQUIRE_OK(submit_status);
  }
  REQUIRE_OK(writer.stop());
  REQUIRE_EQ(writer.metrics().durable_sequence, 100u);
  REQUIRE_OK(writer.start());
  const std::uint8_t last = 100;
  REQUIRE_OK(writer.submit(tfdb::ByteView(&last, 1), 100));
  REQUIRE_OK(writer.checkpoint());
  REQUIRE_OK(writer.stop());
  REQUIRE_EQ(fixture.store->metrics().durable_records, 101u);
  fixture.storage->crash_discard_volatile();
  fixture.store.reset();
  fixture.open(false);
  const auto blocks = collect_blocks(*fixture.store);
  std::vector<std::uint8_t> recovered;
  for (const std::vector<std::uint8_t>& block : blocks)
    recovered.insert(recovered.end(), block.begin(), block.end());
  REQUIRE_EQ(recovered.size(), 101u);
  for (std::uint16_t i = 0; i != 101; ++i)
    REQUIRE_EQ(recovered[i], static_cast<std::uint8_t>(i));
}

TEST(async_stall_has_exact_backpressure_and_durability_age_oracle) {
  Fixture fixture(64u * 1024u, 1, 512);
  fixture.open();
  std::atomic<std::uint64_t> now_ns(10);
  tfdb::AsyncWriterOptions options;
  options.queue_capacity_records = 1;
  options.queue_capacity_bytes = 1;
  options.checkpoint_interval = std::chrono::seconds(10);
  options.monotonic_clock_ns = [&] { return now_ns.load(); };
  tfdb::AsyncWriter writer(*fixture.store, options);
  REQUIRE_OK(writer.start());
  const char a = 'A', b = 'B', c = 'C', d = 'D';
  REQUIRE_OK(writer.submit(tfdb::ByteView(&a, 1), 1));
  const auto processed_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(2);
  while (writer.metrics().processed_records != 1 &&
         std::chrono::steady_clock::now() < processed_deadline)
    std::this_thread::yield();
  REQUIRE_EQ(writer.metrics().processed_records, 1u);

  std::mutex gate_mutex;
  std::condition_variable gate_condition;
  bool backend_blocked = false;
  bool release_backend = false;
  fixture.storage->set_hook([&](tfdb::StorageOperation operation,
                                tfdb::HookPoint point, std::uint64_t,
                                std::uint64_t, std::size_t) {
    if (operation != tfdb::StorageOperation::write ||
        point != tfdb::HookPoint::before) return;
    fixture.storage->set_hook(tfdb::StorageHook());
    std::unique_lock<std::mutex> lock(gate_mutex);
    backend_blocked = true;
    gate_condition.notify_all();
    gate_condition.wait(lock, [&] { return release_backend; });
  });
  now_ns.store(20);
  REQUIRE_OK(writer.submit(tfdb::ByteView(&b, 1), 2));
  {
    std::unique_lock<std::mutex> lock(gate_mutex);
    const bool entered = gate_condition.wait_for(
        lock, std::chrono::seconds(2), [&] { return backend_blocked; });
    if (!entered) {
      release_backend = true;
      gate_condition.notify_all();
    }
    REQUIRE(entered);
  }
  now_ns.store(30);
  REQUIRE_OK(writer.submit(tfdb::ByteView(&c, 1), 3));
  REQUIRE(writer.submit(tfdb::ByteView(&d, 1), 4).code() ==
          tfdb::StatusCode::busy);
  REQUIRE_EQ(writer.queued_records(), 1u);
  REQUIRE_EQ(writer.queued_bytes(), 1u);

  tfdb::Status checkpoint_status;
  std::thread checkpoint_thread([&] { checkpoint_status = writer.checkpoint(); });
  now_ns.store(110);
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    release_backend = true;
  }
  gate_condition.notify_all();
  checkpoint_thread.join();
  REQUIRE_OK(checkpoint_status);
  const tfdb::AsyncWriterMetrics metrics = writer.metrics();
  REQUIRE_EQ(metrics.submitted_records, 3u);
  REQUIRE_EQ(metrics.processed_records, 3u);
  REQUIRE_EQ(metrics.durable_sequence, 3u);
  REQUIRE_EQ(metrics.backpressure_events, 1u);
  REQUIRE_EQ(metrics.maximum_queued_records, 1u);
  REQUIRE_EQ(metrics.maximum_queued_bytes, 1u);
  REQUIRE_EQ(metrics.maximum_queue_residence_ns, 80u);
  REQUIRE_EQ(metrics.maximum_accepted_to_durable_ns, 100u);
  REQUIRE_EQ(metrics.oldest_undurable_age_ns, 0u);
  REQUIRE_OK(writer.stop());

  fixture.storage->crash_discard_volatile();
  fixture.store.reset();
  fixture.open(false);
  const auto blocks = collect_blocks(*fixture.store);
  REQUIRE_EQ(blocks.size(), 3u);
  REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('A'));
  REQUIRE_EQ(blocks[1][0], static_cast<std::uint8_t>('B'));
  REQUIRE_EQ(blocks[2][0], static_cast<std::uint8_t>('C'));
}

TEST(async_custom_clock_drives_timer_and_includes_backend_stall) {
  Fixture fixture;
  fixture.open();
  const std::vector<std::uint8_t> durable_baseline =
      fixture.storage->durable_image();
  fixture.storage->reset_counters();
  const std::uint64_t interval_ns = 1000000;
  const std::uint64_t stall_ns = 2000000;
  std::atomic<std::uint64_t> now_ns(0);
  tfdb::AsyncWriterOptions options;
  options.checkpoint_interval = std::chrono::milliseconds(1);
  options.monotonic_clock_ns = [&] { return now_ns.load(); };
  tfdb::AsyncWriter writer(*fixture.store, options);
  REQUIRE_OK(writer.start());
  const char a = 'A', b = 'B';
  REQUIRE_OK(writer.submit(tfdb::ByteView(&a, 1), 1));
  const auto processed_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(2);
  while (writer.metrics().processed_records != 1 &&
         std::chrono::steady_clock::now() < processed_deadline)
    std::this_thread::yield();
  REQUIRE_EQ(writer.metrics().processed_records, 1u);
  REQUIRE_EQ(fixture.storage->counters().flush_calls, 0u);

  std::mutex gate_mutex;
  std::condition_variable gate_condition;
  bool flush_blocked = false;
  bool release_flush = false;
  fixture.storage->set_hook([&](tfdb::StorageOperation operation,
                                tfdb::HookPoint point, std::uint64_t,
                                std::uint64_t, std::size_t) {
    if (operation != tfdb::StorageOperation::flush ||
        point != tfdb::HookPoint::before) return;
    fixture.storage->set_hook(tfdb::StorageHook());
    std::unique_lock<std::mutex> lock(gate_mutex);
    flush_blocked = true;
    gate_condition.notify_all();
    gate_condition.wait(lock, [&] { return release_flush; });
  });
  now_ns.store(interval_ns);
  writer.notify_clock_advanced();
  {
    std::unique_lock<std::mutex> lock(gate_mutex);
    const bool entered = gate_condition.wait_for(
        lock, std::chrono::seconds(2), [&] { return flush_blocked; });
    if (!entered) {
      release_flush = true;
      gate_condition.notify_all();
    }
    REQUIRE(entered);
  }
  tfdb::AsyncWriterMetrics blocked_metrics = writer.metrics();
  REQUIRE_EQ(blocked_metrics.durable_sequence, 0u);
  REQUIRE_EQ(blocked_metrics.oldest_undurable_age_ns, interval_ns);
  REQUIRE(fixture.storage->durable_image() == durable_baseline);
  REQUIRE_OK(writer.submit(tfdb::ByteView(&b, 1), 2));

  now_ns.store(interval_ns + stall_ns);
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    release_flush = true;
  }
  gate_condition.notify_all();
  const auto durable_deadline = std::chrono::steady_clock::now() +
                                std::chrono::seconds(2);
  while (writer.metrics().durable_sequence < 1 &&
         std::chrono::steady_clock::now() < durable_deadline)
    std::this_thread::yield();
  REQUIRE_EQ(writer.metrics().durable_sequence, 1u);
  REQUIRE_OK(writer.checkpoint());
  const tfdb::AsyncWriterMetrics metrics = writer.metrics();
  REQUIRE_EQ(metrics.durable_sequence, 2u);
  REQUIRE_EQ(metrics.maximum_queue_residence_ns, stall_ns);
  REQUIRE_EQ(metrics.maximum_accepted_to_durable_ns,
             interval_ns + stall_ns);
  REQUIRE_EQ(metrics.oldest_undurable_age_ns, 0u);
  REQUIRE_OK(writer.stop());

  fixture.storage->crash_discard_volatile();
  fixture.store.reset();
  fixture.open(false);
  const auto blocks = collect_blocks(*fixture.store);
  REQUIRE_EQ(blocks.size(), 2u);
  REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('A'));
  REQUIRE_EQ(blocks[1][0], static_cast<std::uint8_t>('B'));
}

TEST(async_custom_timer_flush_failure_never_claims_durability) {
  Fixture fixture;
  fixture.open();
  fixture.storage->reset_counters();
  fixture.storage->add_fault({tfdb::StorageOperation::flush, 1, 0,
                              tfdb::StatusCode::io_error});
  std::atomic<std::uint64_t> now_ns(0);
  tfdb::AsyncWriterOptions options;
  options.checkpoint_interval = std::chrono::milliseconds(1);
  options.monotonic_clock_ns = [&] { return now_ns.load(); };
  tfdb::AsyncWriter writer(*fixture.store, options);
  REQUIRE_OK(writer.start());
  const char value = 'E';
  REQUIRE_OK(writer.submit(tfdb::ByteView(&value, 1), 1));
  const auto processed_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(2);
  while (writer.metrics().processed_records != 1 &&
         std::chrono::steady_clock::now() < processed_deadline)
    std::this_thread::yield();
  REQUIRE_EQ(writer.metrics().processed_records, 1u);
  now_ns.store(1000000);
  writer.notify_clock_advanced();
  const auto failure_deadline = std::chrono::steady_clock::now() +
                                std::chrono::seconds(2);
  while (writer.background_status().ok() &&
         std::chrono::steady_clock::now() < failure_deadline)
    std::this_thread::yield();
  REQUIRE(writer.background_status().code() == tfdb::StatusCode::io_error);
  const char later = 'L';
  REQUIRE(writer.submit(tfdb::ByteView(&later, 1), 2).code() ==
          tfdb::StatusCode::io_error);
  REQUIRE(writer.checkpoint().code() == tfdb::StatusCode::io_error);
  const tfdb::AsyncWriterMetrics metrics = writer.metrics();
  REQUIRE_EQ(metrics.durable_sequence, 0u);
  REQUIRE_EQ(metrics.maximum_accepted_to_durable_ns, 0u);
  REQUIRE_EQ(metrics.background_errors, 1u);
  REQUIRE(writer.stop().code() == tfdb::StatusCode::io_error);
  fixture.storage->crash_discard_volatile();
  fixture.storage->clear_faults();
  fixture.store.reset();
  fixture.open(false);
  REQUIRE(collect_blocks(*fixture.store).empty());
}

TEST(async_rejects_unrepresentable_timer_interval) {
  Fixture fixture;
  fixture.open();
  tfdb::AsyncWriterOptions options;
  options.checkpoint_interval = std::chrono::milliseconds::max();
  tfdb::AsyncWriter writer(*fixture.store, options);
  REQUIRE(writer.start().code() == tfdb::StatusCode::invalid_argument);

  options.checkpoint_interval = std::chrono::milliseconds(
      UINT64_MAX / static_cast<std::uint64_t>(1000000));
  tfdb::AsyncWriter largest_representable(*fixture.store, options);
  REQUIRE_OK(largest_representable.start());
  REQUIRE_OK(largest_representable.stop());
}

TEST(async_timer_at_uint64_max_checkpoints_once_without_busy_loop) {
  Fixture fixture;
  fixture.open();
  constexpr std::uint64_t interval_ns = 1000000;
  std::atomic<std::uint64_t> now_ns(UINT64_MAX - interval_ns);
  tfdb::AsyncWriterOptions options;
  options.checkpoint_interval = std::chrono::milliseconds(1);
  options.monotonic_clock_ns = [&] { return now_ns.load(); };
  tfdb::AsyncWriter writer(*fixture.store, options);
  REQUIRE_OK(writer.start());
  const char value = 'M';
  REQUIRE_OK(writer.submit(tfdb::ByteView(&value, 1), 1));
  const auto processed_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(2);
  while (writer.metrics().processed_records != 1 &&
         std::chrono::steady_clock::now() < processed_deadline)
    std::this_thread::yield();
  REQUIRE_EQ(writer.metrics().processed_records, 1u);

  now_ns.store(UINT64_MAX);
  writer.notify_clock_advanced();
  const auto durable_deadline = std::chrono::steady_clock::now() +
                                std::chrono::seconds(2);
  while (writer.metrics().durable_sequence != 1 &&
         std::chrono::steady_clock::now() < durable_deadline)
    std::this_thread::yield();
  REQUIRE_EQ(writer.metrics().durable_sequence, 1u);
  REQUIRE_EQ(writer.metrics().checkpoint_calls, 1u);
  for (unsigned i = 0; i != 1000; ++i) {
    writer.notify_clock_advanced();
    std::this_thread::yield();
  }
  REQUIRE_EQ(writer.metrics().checkpoint_calls, 1u);
  REQUIRE_OK(writer.stop());
}

TEST(async_sparse_timer_publishes_partial_block) {
  Fixture fixture;
  fixture.open();
  tfdb::AsyncWriterOptions options;
  options.checkpoint_interval = std::chrono::milliseconds(10);
  tfdb::AsyncWriter writer(*fixture.store, options);
  fixture.storage->reset_counters();
  REQUIRE_OK(writer.start());
  const char value = 'q';
  REQUIRE_OK(writer.submit(tfdb::ByteView(&value, 1), 1));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (fixture.storage->counters().flush_calls == 0 &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  REQUIRE(fixture.storage->counters().flush_calls >= 1);
  REQUIRE_EQ(fixture.store->metrics().durable_records, 1u);
  REQUIRE_OK(writer.stop());
  fixture.storage->crash_discard_volatile();
  fixture.store.reset();
  fixture.open(false);
  REQUIRE_EQ(collect_blocks(*fixture.store).size(), 1u);
}

TEST(async_background_failure_is_joined_and_reported) {
  Fixture fixture;
  fixture.open();
  fixture.storage->reset_counters();
  tfdb::FaultRule fault;
  fault.operation = tfdb::StorageOperation::flush;
  fault.call = 1;
  fault.result = tfdb::StatusCode::no_space;
  fixture.storage->add_fault(fault);
  tfdb::AsyncWriterOptions options;
  options.checkpoint_interval = std::chrono::seconds(1);
  tfdb::AsyncWriter writer(*fixture.store, options);
  REQUIRE_OK(writer.start());
  const char value = 'e';
  REQUIRE_OK(writer.submit(tfdb::ByteView(&value, 1), 1));
  const tfdb::Status original = writer.checkpoint();
  REQUIRE(original.code() == tfdb::StatusCode::no_space);
  REQUIRE(!original.message().empty());
  const auto require_original = [&](const tfdb::Status& status) {
    REQUIRE(status.code() == original.code());
    REQUIRE(status.message() == original.message());
  };
  require_original(writer.background_status());
  require_original(writer.submit(tfdb::ByteView(&value, 1), 2));
  require_original(writer.checkpoint());
  require_original(writer.stop());
  require_original(writer.start());
  require_original(fixture.store->writer_status());
  tfdb::AsyncWriter fresh_writer(*fixture.store, options);
  require_original(fresh_writer.start());
}

TEST(explicit_close_durably_flushes_tail_and_closes_writer) {
  Fixture fixture;
  fixture.open();
  const char value = 'x';
  REQUIRE_OK(fixture.store->append(tfdb::ByteView(&value, 1), 1));
  REQUIRE_OK(fixture.store->close());
  REQUIRE_OK(fixture.store->close());
  REQUIRE(fixture.store->append(tfdb::ByteView(&value, 1), 2).code() ==
          tfdb::StatusCode::closed);
  REQUIRE(fixture.store->writer_status().code() == tfdb::StatusCode::closed);
  tfdb::AsyncWriterOptions options;
  tfdb::AsyncWriter writer(*fixture.store, options);
  REQUIRE(writer.start().code() == tfdb::StatusCode::closed);
  fixture.storage->crash_discard_volatile();
  fixture.store.reset();
  fixture.open(false);
  REQUIRE_EQ(collect_blocks(*fixture.store).size(), 1u);
}

TEST(deterministic_crash_model_preserves_every_checkpointed_record) {
  Fixture fixture(1024u * 1024u, 256, 512, 4);
  fixture.partition.record_format_id = tfdb::kFramedRecordV1ProfileId;
  fixture.partition.record_format_version = 1;
  fixture.open();
  std::mt19937 random(0x5eed);
  std::vector<std::uint64_t> required;
  std::vector<std::uint64_t> pending;
  for (std::uint64_t i = 1; i <= 500; ++i) {
    const std::uint8_t byte = static_cast<std::uint8_t>(i);
    REQUIRE_OK(tfdb::append_framed_record(*fixture.store, static_cast<std::int64_t>(i),
                                         i, 0, tfdb::ByteView(&byte, 1)));
    pending.push_back(i);
    if (random() % 13 == 0) {
      REQUIRE_OK(fixture.store->checkpoint());
      required.insert(required.end(), pending.begin(), pending.end());
      pending.clear();
    }
    if (random() % 47 == 0) {
      fixture.storage->crash_discard_volatile();
      fixture.store.reset();
      fixture.open();
      pending.clear();
      const auto actual = collect_selectors(*fixture.store, 0, 1000);
      REQUIRE(actual == required);
    }
  }
  REQUIRE_OK(fixture.store->checkpoint());
  required.insert(required.end(), pending.begin(), pending.end());
  REQUIRE(collect_selectors(*fixture.store, 0, 1000) == required);
}

TEST(file_backend_integration_reopens_data) {
  std::ostringstream path_builder;
  path_builder << "/tmp/tfdb-integration-" << ::getpid() << ".tfdb";
  const std::string path = path_builder.str();
  ::unlink(path.c_str());
  std::shared_ptr<tfdb::FileStorage> storage;
  const std::uint64_t partition_size = 64u * 1024u;
  const std::uint64_t total_size = tfdb::internal::kVolumePrefixSize + partition_size * 3;
  REQUIRE_OK(tfdb::FileStorage::create(path, total_size, false, &storage));
  std::shared_ptr<tfdb::FileStorage> competing_writer;
  REQUIRE(tfdb::FileStorage::open_existing(path, true, &competing_writer).code() ==
          tfdb::StatusCode::busy);
  std::shared_ptr<tfdb::FileStorage> concurrent_reader;
  REQUIRE_OK(tfdb::FileStorage::open_existing(path, false, &concurrent_reader));
  concurrent_reader.reset();
  tfdb::VolumeOptions volume;
  volume.partition_size = partition_size;
  volume.index_region_size = 4096;
  volume.max_block_payload = 256;
  volume.persistence_quantum = 4096;
  REQUIRE_OK(tfdb::RingStore::format(*storage, volume));
  tfdb::OpenOptions options;
  options.writable = true;
  std::unique_ptr<tfdb::RingStore> store;
  REQUIRE_OK(tfdb::RingStore::open(storage, options, &store));
  const char value = 'f';
  REQUIRE_OK(store->append(tfdb::ByteView(&value, 1), 1));
  REQUIRE_OK(store->checkpoint());
  store.reset(); storage.reset();
  REQUIRE_OK(tfdb::FileStorage::open_existing(path, false, &storage));
  options.writable = false;
  REQUIRE_OK(tfdb::RingStore::open(storage, options, &store));
  const auto blocks = collect_blocks(*store);
  REQUIRE_EQ(blocks.size(), 1u);
  REQUIRE_EQ(blocks[0][0], static_cast<std::uint8_t>('f'));
  ::unlink(path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  std::string filter;
  if (argc == 3 && std::string(argv[1]) == "--filter") filter = argv[2];
  else if (argc != 1) {
    std::cerr << "usage: tfdb_tests [--filter SUBSTRING]\n";
    return 2;
  }
  std::size_t failures = 0;
  std::size_t selected = 0;
  for (const Test& test : tests()) {
    if (!filter.empty() && std::string(test.first).find(filter) == std::string::npos)
      continue;
    ++selected;
    try {
      test.second();
      std::cout << "PASS " << test.first << '\n';
    } catch (const Failure& failure) {
      ++failures;
      std::cerr << "FAIL " << test.first << ": " << failure.message << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << test.first << ": exception: " << error.what() << '\n';
    }
  }
  if (selected == 0) {
    std::cerr << "no tests matched filter\n";
    return 2;
  }
  std::cout << "RESULT " << (selected - failures) << '/' << selected
            << " passed\n";
  return failures == 0 ? 0 : 1;
}

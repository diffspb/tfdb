// Copyright (c) 2026 Alexander Safronenko
// SPDX-License-Identifier: BSD-2-Clause

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
#include <stdexcept>
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
#include "tfdb/version.hpp"

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

// Models an exception escaping application-supplied code on the background
// writer thread, which before the guard in AsyncWriter::run() reached the
// thread entry point and called std::terminate.
class ThrowingCodec final : public tfdb::CompressionCodec {
 public:
  tfdb::CompressionId id() const override {
    return static_cast<tfdb::CompressionId>(101);
  }
  std::uint16_t version() const override { return 3; }
  std::size_t max_compressed_size(std::size_t input_size) const override {
    return input_size;
  }
  tfdb::Status compress(tfdb::ByteView,
                        std::vector<std::uint8_t>*) const override {
    throw std::runtime_error("injected codec exception");
  }
  tfdb::Status decompress(tfdb::ByteView, std::size_t,
                          std::vector<std::uint8_t>*) const override {
    throw std::runtime_error("injected codec exception");
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

// Keep the test executable as one translation unit so the deliberately tiny
// registry above remains dependency-free. Scenarios live in thematic files
// to keep reviews and fault-injection changes focused.
#include "cases/format_codec.inc"
#include "cases/storage_recovery.inc"
#include "cases/query_rotation.inc"
#include "cases/io_faults.inc"
#include "cases/records_readers.inc"
#include "cases/async_integration.inc"

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

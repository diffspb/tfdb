// Copyright (c) 2026 Alexander Safronenko
// SPDX-License-Identifier: BSD-2-Clause

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "common.hpp"
#include "internal_format.hpp"
#include "tfdb/async_writer.hpp"
#include "tfdb/counting_storage.hpp"
#include "tfdb/record_profile.hpp"
#include "tfdb/ring_store.hpp"
#include "tfdb/storage.hpp"

namespace {

struct Options {
  std::string path;
  std::uint64_t size = 0;
  std::uint64_t records = 100000;
  std::uint64_t seed = 1;
  std::uint64_t checkpoint_records = 10000;
  std::uint64_t sync_ms = 1000;
  std::uint64_t backend_write_chunk = 0;
  std::string profile = "mixed";
  bool overwrite = false;
  bool asynchronous = false;
  tfdb::VolumeOptions volume;
  tfdb::CompressionId compression = tfdb::CompressionId::none;
};

void usage() {
  std::cerr << "usage: tfdb_loadgen PATH --size BYTES [--records N] "
               "[--profile tiny|mixed|burst|sparse|hot-rotation|concurrent-read|"
               "compressible|incompressible|logs|bad-time] "
               "[--compression none|packbits|lz4] [--partition BYTES] [--index BYTES] "
               "[--block BYTES] [--quantum BYTES] [--checkpoint-records N] "
               "[--async --sync-ms N] [--seed N] [--backend-write-chunk N] "
               "[--overwrite]\n";
}

bool parse(int argc, char** argv, Options* options) {
  if (argc < 4) return false;
  options->path = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--overwrite") options->overwrite = true;
    else if (argument == "--async") options->asynchronous = true;
    else if (argument == "--profile" && i + 1 < argc) options->profile = argv[++i];
    else if (argument == "--compression" && i + 1 < argc) {
      const std::string value = argv[++i];
      if (value == "none") options->compression = tfdb::CompressionId::none;
      else if (value == "packbits") options->compression = tfdb::CompressionId::packbits;
      else if (value == "lz4") options->compression = tfdb::CompressionId::lz4_block;
      else return false;
    } else if (i + 1 < argc) {
      std::uint64_t value = 0;
      if (!parse_u64(argv[++i], &value)) return false;
      if (argument == "--size") options->size = value;
      else if (argument == "--records") options->records = value;
      else if (argument == "--seed") options->seed = value;
      else if (argument == "--checkpoint-records") options->checkpoint_records = value;
      else if (argument == "--sync-ms") options->sync_ms = value;
      else if (argument == "--backend-write-chunk" && value != 0)
        options->backend_write_chunk = value;
      else if (argument == "--partition") options->volume.partition_size = value;
      else if (argument == "--index" && value <= UINT32_MAX) options->volume.index_region_size = static_cast<std::uint32_t>(value);
      else if (argument == "--block" && value <= UINT32_MAX) options->volume.max_block_payload = static_cast<std::uint32_t>(value);
      else if (argument == "--quantum" && value <= UINT32_MAX) options->volume.persistence_quantum = static_cast<std::uint32_t>(value);
      else return false;
    } else return false;
  }
  const std::vector<std::string> profiles = {"tiny", "mixed", "burst", "compressible",
      "incompressible", "logs", "bad-time", "sparse", "hot-rotation",
      "concurrent-read"};
  return options->size != 0 && options->records != 0 &&
         std::find(profiles.begin(), profiles.end(), options->profile) != profiles.end();
}

std::uint64_t splitmix64(std::uint64_t value) {
  value += 0x9e3779b97f4a7c15ull;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
  return value ^ (value >> 31);
}

std::vector<std::uint8_t> make_payload(const Options& options,
                                       std::uint64_t ordinal) {
  std::mt19937_64 random(splitmix64(options.seed ^ ordinal));
  std::size_t size = 24;
  if (options.profile == "tiny" || options.profile == "sparse")
    size = 8 + static_cast<std::size_t>(random() % 25);
  else if (options.profile == "mixed" || options.profile == "hot-rotation" ||
           options.profile == "concurrent-read")
    size = ordinal % 100 == 0 ? 50 + static_cast<std::size_t>(random() % 101)
                              : 10 + static_cast<std::size_t>(random() % 40);
  else if (options.profile == "burst") size = 16 + static_cast<std::size_t>(random() % 112);
  else if (options.profile == "compressible") size = 128;
  else if (options.profile == "incompressible") size = 128;
  else if (options.profile == "logs") size = 50 + static_cast<std::size_t>(random() % 300);
  else if (options.profile == "bad-time") size = 32;
  std::vector<std::uint8_t> payload(size);
  if (options.profile == "compressible") {
    std::fill(payload.begin(), payload.end(), static_cast<std::uint8_t>((ordinal / 100) & 3));
  } else if (options.profile == "logs") {
    const std::string prefix = "service=control level=info repeated telemetry log line ";
    for (std::size_t i = 0; i != size; ++i) payload[i] = prefix[i % prefix.size()];
  } else {
    for (std::uint8_t& byte : payload) byte = static_cast<std::uint8_t>(random());
  }
  for (unsigned byte = 0; byte != 8; ++byte)
    payload[byte] = static_cast<std::uint8_t>(ordinal >> (byte * 8));
  return payload;
}

std::int64_t make_time(const Options& options, std::uint64_t ordinal) {
  if (options.profile != "bad-time") return static_cast<std::int64_t>(ordinal * 1000000ull);
  if (ordinal < options.records / 10) return static_cast<std::int64_t>(ordinal * 1000000ull);
  if (ordinal == options.records / 2) return 0;
  const std::uint64_t base = 1780000000000000000ull;
  const std::int64_t mixed = ordinal % 7 == 0 ? -3000000 : 0;
  return static_cast<std::int64_t>(base + ordinal * 1000000ull) + mixed;
}

class BoundedLatencySamples {
 public:
  static constexpr std::size_t kCapacity = 65536;

  BoundedLatencySamples() { values_.reserve(kCapacity); }

  void add(std::uint64_t value) {
    ++observations_;
    maximum_ = std::max(maximum_, value);
    if (values_.size() < kCapacity) {
      values_.push_back(value);
      return;
    }
    // Deterministic reservoir sampling: all observations have equal inclusion
    // probability while memory remains fixed for arbitrarily long profiles.
    const std::uint64_t candidate = splitmix64(observations_) % observations_;
    if (candidate < kCapacity)
      values_[static_cast<std::size_t>(candidate)] = value;
  }

  std::uint64_t percentile(double fraction) const {
    if (values_.empty()) return 0;
    std::vector<std::uint64_t> sorted = values_;
    std::sort(sorted.begin(), sorted.end());
    const std::size_t index = static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(sorted.size()))) - 1;
    return sorted[std::min(index, sorted.size() - 1)];
  }

  std::uint64_t maximum() const { return maximum_; }
  std::uint64_t observations() const { return observations_; }
  std::size_t samples() const { return values_.size(); }

 private:
  std::vector<std::uint64_t> values_;
  std::uint64_t observations_ = 0;
  std::uint64_t maximum_ = 0;
};

struct RecordStreamSummary {
  std::uint64_t count = 0;
  std::uint64_t first_ordinal = 0;
  std::uint64_t last_ordinal = 0;
  std::uint64_t blocks = 0;
  std::uint64_t raw_bytes = 0;
  std::uint64_t gaps = 0;
  std::uint64_t digest = 1469598103934665603ull;
};

bool operator==(const RecordStreamSummary& a, const RecordStreamSummary& b) {
  return a.count == b.count && a.first_ordinal == b.first_ordinal &&
         a.last_ordinal == b.last_ordinal && a.blocks == b.blocks &&
         a.raw_bytes == b.raw_bytes && a.gaps == b.gaps &&
         a.digest == b.digest;
}

void hash_value(std::uint64_t value, std::uint64_t* digest) {
  for (unsigned byte = 0; byte != 8; ++byte) {
    *digest ^= static_cast<std::uint8_t>(value >> (byte * 8));
    *digest *= 1099511628211ull;
  }
}

void hash_bytes(tfdb::ByteView bytes, std::uint64_t* digest) {
  for (std::size_t i = 0; i != bytes.size(); ++i) {
    *digest ^= bytes.data()[i];
    *digest *= 1099511628211ull;
  }
}

// Independent write-set oracle for the load tool. It observes successful
// persistent frames below RingStore, models slot replacement by partition
// headers, and derives the exact records that must remain visible. It does not
// consult RingStore's catalog, index, inspect(), or query implementation.
class ShortWriteStorage final : public tfdb::Storage {
 public:
  ShortWriteStorage(std::shared_ptr<tfdb::Storage> delegate,
                    std::size_t maximum_write)
      : delegate_(std::move(delegate)), maximum_write_(maximum_write) {}
  std::uint64_t size() const override { return delegate_->size(); }
  bool writable() const override { return delegate_->writable(); }
  tfdb::IoResult read_at(std::uint64_t offset,
                         tfdb::MutableByteView output) override {
    return delegate_->read_at(offset, output);
  }
  tfdb::IoResult write_at(std::uint64_t offset, tfdb::ByteView input) override {
    const std::size_t amount = std::min(maximum_write_, input.size());
    return delegate_->write_at(offset, tfdb::ByteView(input.data(), amount));
  }
  tfdb::Status flush() override { return delegate_->flush(); }

 private:
  std::shared_ptr<tfdb::Storage> delegate_;
  std::size_t maximum_write_;
};

class PersistenceOracleStorage final : public tfdb::Storage {
 public:
  PersistenceOracleStorage(std::shared_ptr<tfdb::Storage> delegate,
                           const tfdb::VolumeOptions& volume)
      : delegate_(std::move(delegate)), volume_(volume),
        slots_(static_cast<std::size_t>(
            (delegate_->size() - tfdb::internal::kVolumePrefixSize) /
            volume.partition_size)) {}

  std::uint64_t size() const override { return delegate_->size(); }
  bool writable() const override { return delegate_->writable(); }
  tfdb::IoResult read_at(std::uint64_t offset,
                         tfdb::MutableByteView output) override {
    return delegate_->read_at(offset, output);
  }
  tfdb::IoResult write_at(std::uint64_t offset, tfdb::ByteView input) override {
    tfdb::IoResult result = delegate_->write_at(offset, input);
    if (result.status.ok() && result.transferred != 0) {
      std::lock_guard<std::mutex> lock(mutex_);
      observe_fragment(offset, input, result.transferred);
    }
    return result;
  }
  tfdb::Status flush() override { return delegate_->flush(); }

  tfdb::Status expected_summary(const Options& options,
                                RecordStreamSummary* output) const {
    if (!output)
      return tfdb::Status::Error(tfdb::StatusCode::invalid_argument,
                                 "null persistence-oracle output");
    std::lock_guard<std::mutex> lock(mutex_);
    if (!status_.ok()) return status_;
    if (pending_.active)
      return tfdb::Status::Error(tfdb::StatusCode::corrupt,
                                 "oracle has an incomplete exact write");
    *output = RecordStreamSummary();
    std::vector<const SlotState*> retained;
    for (const SlotState& slot : slots_)
      if (slot.active) retained.push_back(&slot);
    std::sort(retained.begin(), retained.end(),
        [](const SlotState* a, const SlotState* b) {
          return a->generation < b->generation;
        });
    for (std::size_t i = 1; i != retained.size(); ++i) {
      if (retained[i]->generation != retained[i - 1]->generation + 1)
        return tfdb::Status::Error(tfdb::StatusCode::corrupt,
                                   "oracle retained generations are not contiguous");
    }
    for (const SlotState* slot : retained) {
      for (const ObservedBlock& block : slot->blocks) {
        if (output->count == 0) output->first_ordinal = block.first_ordinal;
        if (output->count != 0 &&
            block.first_ordinal != output->last_ordinal + 1)
          return tfdb::Status::Error(tfdb::StatusCode::corrupt,
                                     "oracle retained records are not contiguous");
        ++output->blocks;
        output->raw_bytes += block.raw_size;
        hash_value(slot->generation, &output->digest);
        hash_value(block.sequence, &output->digest);
        hash_value(block.slot, &output->digest);
        hash_value(block.flags, &output->digest);
        for (std::uint64_t ordinal = block.first_ordinal;; ++ordinal) {
          const std::vector<std::uint8_t> payload = make_payload(options, ordinal);
          std::vector<std::uint8_t> encoded;
          tfdb::Status encoded_status = tfdb::FramedRecordV1::encode(
              make_time(options, ordinal), ordinal % 64, 0,
              tfdb::ByteView(payload), &encoded);
          if (!encoded_status.ok()) return encoded_status;
          hash_bytes(tfdb::ByteView(encoded), &output->digest);
          ++output->count;
          output->last_ordinal = ordinal;
          if (ordinal == block.last_ordinal) break;
        }
      }
    }
    if (output->count == 0 || output->last_ordinal != options.records - 1)
      return tfdb::Status::Error(tfdb::StatusCode::corrupt,
                                 "oracle retained window is empty or stale");
    return tfdb::Status::Ok();
  }

 private:
  struct ObservedBlock {
    std::uint32_t slot = 0;
    std::uint32_t sequence = 0;
    std::uint32_t flags = 0;
    std::uint32_t raw_size = 0;
    std::uint64_t first_ordinal = 0;
    std::uint64_t last_ordinal = 0;
  };
  struct SlotState {
    bool active = false;
    std::uint64_t generation = 0;
    std::uint64_t data_end = tfdb::internal::kPartitionHeaderRegionSize;
    std::vector<ObservedBlock> blocks;
  };
  struct PendingWrite {
    bool active = false;
    std::uint64_t offset = 0;
    std::size_t expected_size = 0;
    std::vector<std::uint8_t> bytes;
  };

  std::uint64_t partition_base(std::uint32_t slot) const {
    return tfdb::internal::kVolumePrefixSize +
           static_cast<std::uint64_t>(slot) * volume_.partition_size;
  }

  void fail(const char* message) {
    if (status_.ok())
      status_ = tfdb::Status::Error(tfdb::StatusCode::corrupt, message);
  }

  void observe_fragment(std::uint64_t offset, tfdb::ByteView requested,
                        std::size_t transferred) {
    if (!status_.ok()) return;
    if (transferred > requested.size()) {
      fail("oracle backend over-reported a write fragment");
      return;
    }
    if (!pending_.active && transferred == requested.size()) {
      observe_write(offset, tfdb::ByteView(requested.data(), transferred));
      return;
    }
    if (!pending_.active) {
      pending_.active = true;
      pending_.offset = offset;
      pending_.expected_size = requested.size();
      pending_.bytes.assign(requested.data(), requested.data() + transferred);
      return;
    }
    if (offset != pending_.offset + pending_.bytes.size() ||
        requested.size() != pending_.expected_size - pending_.bytes.size() ||
        transferred > pending_.expected_size - pending_.bytes.size()) {
      fail("oracle saw noncontiguous fragments of one exact write");
      return;
    }
    pending_.bytes.insert(pending_.bytes.end(), requested.data(),
                          requested.data() + transferred);
    if (pending_.bytes.size() == pending_.expected_size) {
      const std::uint64_t complete_offset = pending_.offset;
      std::vector<std::uint8_t> complete;
      complete.swap(pending_.bytes);
      pending_ = PendingWrite();
      observe_write(complete_offset, tfdb::ByteView(complete));
    }
  }

  void observe_write(std::uint64_t offset, tfdb::ByteView input) {
    if (!status_.ok()) return;
    for (std::uint32_t slot = 0; slot != slots_.size(); ++slot) {
      const std::uint64_t base = partition_base(slot);
      if (offset == base &&
          input.size() == tfdb::internal::kPartitionHeaderEncodedSize) {
        tfdb::internal::PartitionHeader header;
        const tfdb::Status decoded =
            tfdb::internal::decode_partition_header(input, &header);
        if (!decoded.ok() || header.slot != slot || header.generation == 0) {
          fail("oracle saw an invalid partition header write");
          return;
        }
        if ((!have_generation_ && (header.generation != 1 || slot != 0)) ||
            (have_generation_ &&
             (header.generation != latest_generation_ + 1 ||
              static_cast<std::size_t>(slot) !=
                  (static_cast<std::size_t>(latest_slot_) + 1u) %
                      slots_.size()))) {
          fail("oracle saw a nonsequential partition replacement");
          return;
        }
        SlotState fresh;
        fresh.active = true;
        fresh.generation = header.generation;
        slots_[slot] = fresh;
        have_generation_ = true;
        latest_generation_ = header.generation;
        latest_slot_ = slot;
        return;
      }
    }

    const std::uint64_t index_begin = volume_.partition_size -
        tfdb::internal::kPartitionFooterRegionSize - volume_.index_region_size;
    for (std::uint32_t slot = 0; slot != slots_.size(); ++slot) {
      SlotState& state = slots_[slot];
      if (!state.active) continue;
      const std::uint64_t base = partition_base(slot);
      if (state.data_end < index_begin && offset == base + state.data_end) {
        if (input.size() < tfdb::internal::kBlockHeaderEncodedSize) {
          fail("oracle saw a truncated block frame write");
          return;
        }
        tfdb::internal::BlockHeader header;
        const tfdb::Status decoded = tfdb::internal::decode_block_header(
            tfdb::ByteView(input.data(),
                           tfdb::internal::kBlockHeaderEncodedSize), &header);
        bool span_overflow = false;
        const std::uint64_t expected_span = tfdb::internal::align_up(
            input.size(), volume_.persistence_quantum, &span_overflow);
        if (!decoded.ok()) { fail("oracle saw an invalid block header"); return; }
        if (header.slot != slot || header.generation != state.generation) {
          fail("oracle block identity differs from current partition"); return;
        }
        if (header.sequence != state.blocks.size()) {
          fail("oracle block sequence is not contiguous"); return;
        }
        if (static_cast<std::uint64_t>(header.stored_size) +
                tfdb::internal::kBlockHeaderEncodedSize != input.size()) {
          fail("oracle block write length differs from its header"); return;
        }
        if (header.frame_span != expected_span || span_overflow) {
          fail("oracle block frame span differs from aligned write length"); return;
        }
        if (state.data_end > index_begin ||
            header.frame_span > index_begin - state.data_end) {
          fail("oracle block crosses the reserved index region"); return;
        }
        ObservedBlock block;
        block.slot = slot;
        block.sequence = header.sequence;
        block.flags = header.flags;
        block.raw_size = header.raw_size;
        block.first_ordinal = next_written_ordinal_;
        if (header.record_count == 0 ||
            static_cast<std::uint64_t>(header.record_count) >
                UINT64_MAX - next_written_ordinal_) {
          fail("oracle block record count overflows the generated sequence");
          return;
        }
        block.last_ordinal = next_written_ordinal_ + header.record_count - 1u;
        next_written_ordinal_ += header.record_count;
        state.blocks.push_back(block);
        state.data_end += header.frame_span;
        return;
      }
      if (offset > base && offset < base + index_begin) {
        fail("oracle saw a nonsequential data-region write");
        return;
      }
    }
  }

  std::shared_ptr<tfdb::Storage> delegate_;
  tfdb::VolumeOptions volume_;
  mutable std::mutex mutex_;
  std::vector<SlotState> slots_;
  bool have_generation_ = false;
  std::uint64_t latest_generation_ = 0;
  std::uint32_t latest_slot_ = 0;
  std::uint64_t next_written_ordinal_ = 0;
  PendingWrite pending_;
  tfdb::Status status_;
};

tfdb::Status collect_records(const tfdb::RingStore& store,
                             const Options& options,
                             RecordStreamSummary* output) {
  *output = RecordStreamSummary();
  tfdb::FramedRecordV1 profile;
  tfdb::RecordQuery query;
  query.time.begin_ns = std::numeric_limits<std::int64_t>::min();
  query.time.end_ns = std::numeric_limits<std::int64_t>::max();
  tfdb::Status validation;
  bool have_block = false;
  std::uint64_t block_generation = 0;
  std::uint32_t block_sequence = 0;
  tfdb::Status status = tfdb::query_records(store, query, profile,
      [&](const tfdb::RecordEvent& event) {
        if (event.kind != tfdb::BlockEventKind::data) {
          ++output->gaps;
          validation = tfdb::Status::Error(tfdb::StatusCode::corrupt,
                                           "record stream contains a gap");
          return false;
        }
        if (event.record.payload.size() < 8) {
          validation = tfdb::Status::Error(tfdb::StatusCode::corrupt,
                                           "record payload lacks ordinal");
          return false;
        }
        std::uint64_t ordinal = 0;
        for (unsigned byte = 0; byte != 8; ++byte)
          ordinal |= static_cast<std::uint64_t>(
              event.record.payload.data()[byte]) << (byte * 8);
        if (ordinal >= options.records ||
            (output->count != 0 && ordinal != output->last_ordinal + 1) ||
            event.record.selector != ordinal % 64 ||
            event.record.index_time_ns != make_time(options, ordinal) ||
            event.record.flags != 0) {
          validation = tfdb::Status::Error(
              tfdb::StatusCode::corrupt,
              "record identity/order differs from generated model");
          return false;
        }
        const std::vector<std::uint8_t> expected_payload =
            make_payload(options, ordinal);
        std::vector<std::uint8_t> expected_encoded;
        validation = tfdb::FramedRecordV1::encode(
            make_time(options, ordinal), ordinal % 64, 0,
            tfdb::ByteView(expected_payload), &expected_encoded);
        if (!validation.ok() ||
            expected_encoded.size() != event.record.encoded.size() ||
            !std::equal(expected_encoded.begin(), expected_encoded.end(),
                        event.record.encoded.data())) {
          validation = tfdb::Status::Error(
              tfdb::StatusCode::corrupt,
              "record bytes differ from generated model");
          return false;
        }
        if (!have_block ||
            event.block.partition_generation != block_generation ||
            event.block.block_sequence != block_sequence) {
          if (have_block &&
              (event.block.partition_generation < block_generation ||
               (event.block.partition_generation == block_generation &&
                event.block.block_sequence <= block_sequence))) {
            validation = tfdb::Status::Error(
                tfdb::StatusCode::corrupt,
                "block identity is not in physical generation order");
            return false;
          }
          have_block = true;
          block_generation = event.block.partition_generation;
          block_sequence = event.block.block_sequence;
          ++output->blocks;
          output->raw_bytes += event.block.raw_size;
          hash_value(block_generation, &output->digest);
          hash_value(block_sequence, &output->digest);
          hash_value(event.block.partition_slot, &output->digest);
          hash_value(event.block.flags, &output->digest);
        }
        if (output->count == 0) output->first_ordinal = ordinal;
        output->last_ordinal = ordinal;
        ++output->count;
        hash_bytes(event.record.encoded, &output->digest);
        return true;
      });
  if (!status.ok()) return status;
  if (!validation.ok()) return validation;
  if (output->count == 0 || output->last_ordinal != options.records - 1)
    return tfdb::Status::Error(tfdb::StatusCode::corrupt,
                               "retained record suffix is empty or stale");
  return tfdb::Status::Ok();
}

tfdb::Status measure_record_query(const tfdb::RingStore& store,
                                  RecordStreamSummary* output) {
  if (!output)
    return tfdb::Status::Error(tfdb::StatusCode::invalid_argument,
                               "null query measurement output");
  *output = RecordStreamSummary();
  tfdb::FramedRecordV1 profile;
  tfdb::RecordQuery query;
  query.time.begin_ns = std::numeric_limits<std::int64_t>::min();
  query.time.end_ns = std::numeric_limits<std::int64_t>::max();
  tfdb::Status validation;
  bool have_block = false;
  std::uint64_t generation = 0;
  std::uint32_t sequence = 0;
  const tfdb::Status status = tfdb::query_records(
      store, query, profile, [&](const tfdb::RecordEvent& event) {
        if (event.kind != tfdb::BlockEventKind::data) {
          ++output->gaps;
          validation = tfdb::Status::Error(tfdb::StatusCode::corrupt,
                                           "measured query contains a gap");
          return false;
        }
        if (!have_block || event.block.partition_generation != generation ||
            event.block.block_sequence != sequence) {
          have_block = true;
          generation = event.block.partition_generation;
          sequence = event.block.block_sequence;
          ++output->blocks;
          output->raw_bytes += event.block.raw_size;
          hash_value(generation, &output->digest);
          hash_value(sequence, &output->digest);
          hash_value(event.block.partition_slot, &output->digest);
          hash_value(event.block.flags, &output->digest);
        }
        ++output->count;
        hash_bytes(event.record.encoded, &output->digest);
        return true;
      });
  if (!status.ok()) return status;
  return validation;
}

double ratio(std::uint64_t numerator, std::uint64_t denominator) {
  return denominator == 0 ? 0.0 : static_cast<double>(numerator) /
                                   static_cast<double>(denominator);
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse(argc, argv, &options)) { usage(); return 1; }
  std::shared_ptr<tfdb::FileStorage> file;
  tfdb::Status status = tfdb::FileStorage::create(options.path, options.size,
                                                   options.overwrite, &file);
  if (!status.ok()) return print_status(status);
  status = tfdb::RingStore::format(*file, options.volume);
  if (!status.ok()) return print_status(status);
  std::shared_ptr<tfdb::Storage> file_base = file;
  if (options.backend_write_chunk != 0) {
    if (options.backend_write_chunk >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      std::cerr << "backend write chunk exceeds size_t\n";
      return 1;
    }
    file_base.reset(new ShortWriteStorage(
        file_base, static_cast<std::size_t>(options.backend_write_chunk)));
  }
  std::shared_ptr<PersistenceOracleStorage> persistence_oracle(
      new PersistenceOracleStorage(file_base, options.volume));
  std::shared_ptr<tfdb::Storage> observed_storage = persistence_oracle;
  std::shared_ptr<tfdb::CountingStorage> storage(
      new tfdb::CountingStorage(observed_storage));
  tfdb::OpenOptions open;
  open.writable = true;
  open.next_partition.compression = options.compression;
  open.next_partition.record_format_id = tfdb::kFramedRecordV1ProfileId;
  open.next_partition.record_format_version = 1;
  open.next_partition.allowed_backward_skew_ns = 10000000;
  open.next_partition.allowed_forward_step_ns = 1000000000;
  std::unique_ptr<tfdb::RingStore> store;
  status = tfdb::RingStore::open(storage, open, &store);
  if (!status.ok()) return print_status(status);
  storage->reset_counters();

  std::unique_ptr<tfdb::AsyncWriter> async;
  if (options.asynchronous) {
    tfdb::AsyncWriterOptions async_options;
    async_options.queue_capacity_records = 8192;
    async_options.queue_capacity_bytes = 16u * 1024u * 1024u;
    async_options.checkpoint_interval = std::chrono::milliseconds(options.sync_ms);
    async.reset(new tfdb::AsyncWriter(*store, async_options));
    status = async->start();
    if (!status.ok()) return print_status(status);
  }

  std::atomic<bool> reader_stop(false), reader_failed(false);
  std::atomic<std::uint64_t> reader_iterations(0), reader_gaps(0);
  std::atomic<std::uint64_t> reader_data_events(0), reader_records(0);
  std::thread concurrent_reader;
  if (options.profile == "concurrent-read") {
    concurrent_reader = std::thread([&] {
      while (!reader_stop.load()) {
        const tfdb::Status read_status = store->scan_blocks(
            tfdb::QueryOptions(), [&](const tfdb::BlockEvent& event) {
              if (event.kind == tfdb::BlockEventKind::corrupt_gap)
                reader_failed.store(true);
              else if (event.kind == tfdb::BlockEventKind::overwritten_gap)
                ++reader_gaps;
              else {
                ++reader_data_events;
                tfdb::FramedRecordV1 profile;
                std::uint32_t decoded = 0;
                const tfdb::Status decoded_status = profile.decode_block(
                    event.data, [&](const tfdb::RecordView& record) {
                      ++decoded;
                      if (record.index_time_ns < 0 ||
                          record.index_time_ns % 1000000 != 0 ||
                          record.payload.size() < 8) {
                        reader_failed.store(true);
                        return true;
                      }
                      const std::uint64_t ordinal = static_cast<std::uint64_t>(
                          record.index_time_ns / 1000000);
                      std::uint64_t encoded_ordinal = 0;
                      for (unsigned byte = 0; byte != 8; ++byte)
                        encoded_ordinal |= static_cast<std::uint64_t>(
                            record.payload.data()[byte]) << (byte * 8);
                      if (ordinal >= options.records ||
                          record.selector != ordinal % 64 ||
                          encoded_ordinal != ordinal ||
                          record.index_time_ns != make_time(options, ordinal) ||
                          record.flags != 0)
                        reader_failed.store(true);
                      const std::vector<std::uint8_t> expected =
                          make_payload(options, ordinal);
                      if (expected.size() != record.payload.size() ||
                          !std::equal(expected.begin(), expected.end(),
                                      record.payload.data()))
                        reader_failed.store(true);
                      std::vector<std::uint8_t> expected_encoded;
                      const tfdb::Status encoded_status =
                          tfdb::FramedRecordV1::encode(
                              make_time(options, ordinal), ordinal % 64, 0,
                              tfdb::ByteView(expected), &expected_encoded);
                      if (!encoded_status.ok() ||
                          expected_encoded.size() != record.encoded.size() ||
                          !std::equal(expected_encoded.begin(),
                                      expected_encoded.end(),
                                      record.encoded.data()))
                        reader_failed.store(true);
                      ++reader_records;
                      return true;
                    });
                if (!decoded_status.ok() || decoded != event.metadata.record_count)
                  reader_failed.store(true);
              }
              return true;
            });
        if (!read_status.ok()) reader_failed.store(true);
        ++reader_iterations;
      }
    });
  }

  BoundedLatencySamples append_latencies;
  BoundedLatencySamples checkpoint_latencies;
  std::uint64_t logical_payload_bytes = 0;
  std::uint64_t backpressure = 0;
  const auto write_start = std::chrono::steady_clock::now();
  for (std::uint64_t i = 0; i != options.records; ++i) {
    std::vector<std::uint8_t> payload = make_payload(options, i);
    logical_payload_bytes += payload.size();
    std::vector<std::uint8_t> encoded;
    const std::int64_t time = make_time(options, i);
    status = tfdb::FramedRecordV1::encode(time, i % 64, 0, tfdb::ByteView(payload), &encoded);
    if (!status.ok()) return print_status(status);
    const auto begin = std::chrono::steady_clock::now();
    if (async) {
      do {
        status = async->submit(tfdb::ByteView(encoded), time);
        if (status.code() == tfdb::StatusCode::busy) {
          ++backpressure;
          std::this_thread::yield();
        }
      } while (status.code() == tfdb::StatusCode::busy);
    } else {
      status = store->append(tfdb::ByteView(encoded), time);
    }
    const auto end = std::chrono::steady_clock::now();
    append_latencies.add(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count()));
    if (!status.ok()) return print_status(status);
    if (!async && options.checkpoint_records != 0 &&
        (i + 1) % options.checkpoint_records == 0) {
      const auto checkpoint_begin = std::chrono::steady_clock::now();
      status = store->checkpoint();
      const auto checkpoint_end = std::chrono::steady_clock::now();
      checkpoint_latencies.add(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              checkpoint_end - checkpoint_begin).count()));
      if (!status.ok()) return print_status(status);
    }
  }
  status = async ? async->checkpoint() : store->checkpoint();
  if (!status.ok()) return print_status(status);
  tfdb::AsyncWriterMetrics async_metrics;
  if (async) {
    status = async->stop();
    if (!status.ok()) return print_status(status);
    async_metrics = async->metrics();
  }
  reader_stop.store(true);
  if (concurrent_reader.joinable()) concurrent_reader.join();
  if (reader_failed.load()) {
    std::cerr << "concurrent reader observed corruption/error\n";
    return 3;
  }
  if (options.profile == "concurrent-read" &&
      (reader_iterations.load() == 0 || reader_data_events.load() == 0 ||
       reader_records.load() == 0)) {
    std::cerr << "concurrent reader oracle was vacuous\n";
    return 3;
  }
  const auto write_end = std::chrono::steady_clock::now();
  const double write_seconds = std::chrono::duration<double>(write_end - write_start).count();
  const tfdb::StoreMetrics metrics = store->metrics();
  const tfdb::StorageCounters write_io = storage->counters();
  tfdb::VolumeInfo info;
  status = store->inspect(&info);
  if (!status.ok()) return print_status(status);
  std::uint64_t occupied_span = 0;
  for (const tfdb::PartitionInfo& partition : info.partitions) occupied_span += partition.data_bytes;

  storage->reset_counters();
  RecordStreamSummary measured_query;
  const auto query_start = std::chrono::steady_clock::now();
  status = measure_record_query(*store, &measured_query);
  const auto query_end = std::chrono::steady_clock::now();
  if (!status.ok()) return print_status(status);
  const tfdb::StorageCounters query_io = storage->counters();

  // Full deterministic content/retention validation is deliberately outside
  // the measured query interval and its I/O counters.
  RecordStreamSummary queried_records;
  status = collect_records(*store, options, &queried_records);
  if (!status.ok()) return print_status(status);
  if (measured_query.count != queried_records.count ||
      measured_query.blocks != queried_records.blocks ||
      measured_query.raw_bytes != queried_records.raw_bytes ||
      measured_query.gaps != queried_records.gaps ||
      measured_query.digest != queried_records.digest) {
    std::cerr << "measured query differs from strict verification pass\n";
    return 3;
  }
  RecordStreamSummary expected_records;
  status = persistence_oracle->expected_summary(options, &expected_records);
  if (!status.ok()) return print_status(status);
  if (!(queried_records == expected_records)) {
    std::cerr << "query differs from independent persisted-write oracle\n";
    return 3;
  }

  store.reset();
  storage->reset_counters();
  open.writable = false;
  const auto recovery_start = std::chrono::steady_clock::now();
  status = tfdb::RingStore::open(storage, open, &store);
  const auto recovery_end = std::chrono::steady_clock::now();
  if (!status.ok()) return print_status(status);
  const tfdb::StorageCounters recovery_io = storage->counters();
  RecordStreamSummary reopened_records;
  status = collect_records(*store, options, &reopened_records);
  if (!status.ok()) return print_status(status);
  if (!(reopened_records == expected_records) ||
      !(reopened_records == queried_records)) {
    std::cerr << "verification mismatch/gap before or after reopen\n";
    return 3;
  }

  std::cout << "profile=" << options.profile
            << " seed=" << options.seed
            << " records=" << options.records
            << " logical_payload_bytes=" << logical_payload_bytes
            << " framed_input_bytes=" << metrics.accepted_payload_bytes
            << " seconds=" << write_seconds
            << " records_per_second="
            << (write_seconds == 0.0 ? 0.0 :
                static_cast<double>(options.records) / write_seconds)
            << " append_p50_ns=" << append_latencies.percentile(0.50)
            << " append_p95_ns=" << append_latencies.percentile(0.95)
            << " append_p99_ns=" << append_latencies.percentile(0.99)
            << " append_observed_max_ns=" << append_latencies.maximum()
            << " append_latency_observations=" << append_latencies.observations()
            << " append_latency_samples=" << append_latencies.samples()
            << " checkpoint_p50_ns=" << checkpoint_latencies.percentile(0.50)
            << " checkpoint_p95_ns=" << checkpoint_latencies.percentile(0.95)
            << " checkpoint_observed_max_ns="
            << checkpoint_latencies.maximum()
            << " checkpoint_latency_observations="
            << checkpoint_latencies.observations()
            << " checkpoint_latency_samples=" << checkpoint_latencies.samples()
            << " backend_write_calls=" << write_io.write_calls
            << " backend_write_bytes=" << write_io.write_bytes
            << " backend_flush_calls=" << write_io.flush_calls
            << " host_write_amplification=" << ratio(write_io.write_bytes, metrics.accepted_payload_bytes)
            << " logical_payload_per_occupied_span="
            << ratio(logical_payload_bytes, occupied_span)
            << " compression_ratio=" << ratio(metrics.stored_block_bytes, metrics.raw_block_bytes)
            << " blocks=" << metrics.published_blocks
            << " rotations=" << metrics.rotations
            << " checkpoints=" << metrics.checkpoints
            << " durable_records=" << metrics.durable_records
            << " sync_calls=" << metrics.sync_calls
            << " max_sync_ns=" << metrics.max_sync_duration_ns
            << " max_accept_to_durable_ns=" << metrics.max_accepted_to_durable_ns
            << " anomalies=" << metrics.time_anomalies
            << " backpressure_events=" << backpressure
            << " async_max_queue_records=" << async_metrics.maximum_queued_records
            << " async_max_queue_bytes=" << async_metrics.maximum_queued_bytes
            << " async_max_queue_residence_ns=" << async_metrics.maximum_queue_residence_ns
            << " async_max_accept_to_durable_ns=" << async_metrics.maximum_accepted_to_durable_ns
            << " concurrent_reader_iterations=" << reader_iterations.load()
            << " concurrent_reader_overwrite_gaps=" << reader_gaps.load()
            << " concurrent_reader_data_events=" << reader_data_events.load()
            << " concurrent_reader_records=" << reader_records.load()
            << " queried_blocks=" << queried_records.blocks
            << " queried_raw_bytes=" << queried_records.raw_bytes
            << " retained_records=" << queried_records.count
            << " retained_first_ordinal=" << queried_records.first_ordinal
            << " retained_last_ordinal=" << queried_records.last_ordinal
            << " retained_digest=" << queried_records.digest
            << " query_seconds=" << std::chrono::duration<double>(query_end - query_start).count()
            << " query_backend_read_bytes=" << query_io.read_bytes
            << " recovery_seconds=" << std::chrono::duration<double>(recovery_end - recovery_start).count()
            << " recovery_backend_read_bytes=" << recovery_io.read_bytes
            << '\n';
  return 0;
}

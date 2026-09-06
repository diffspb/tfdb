#include "tfdb/ring_store.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <set>
#include <sstream>
#include <cerrno>
#include <cstring>
#include <sys/random.h>
#include <unistd.h>

#include "internal_format.hpp"

namespace tfdb {
namespace {

using internal::BlockHeader;
using internal::IndexEntry;
using internal::PartitionFooter;
using internal::PartitionHeader;
using internal::VolumeHeader;

bool same_volume_header(const VolumeHeader& a, const VolumeHeader& b) {
  return a.volume_id_high == b.volume_id_high &&
         a.volume_id_low == b.volume_id_low &&
         a.volume_size == b.volume_size &&
         a.partition_size == b.partition_size &&
         a.partition_count == b.partition_count &&
         a.index_region_size == b.index_region_size &&
         a.max_block_payload == b.max_block_payload &&
         a.persistence_quantum == b.persistence_quantum &&
         a.created_time_ns == b.created_time_ns;
}

bool is_power_of_two(std::uint32_t value) {
  return value != 0 && (value & (value - 1u)) == 0;
}

Status validate_volume_geometry(const VolumeHeader& volume,
                                std::uint64_t backend_size) {
  if (volume.volume_id_high == 0 && volume.volume_id_low == 0)
    return Status::Error(StatusCode::corrupt, "zero volume incarnation ID");
  if (volume.volume_size > backend_size ||
      volume.volume_size < internal::kVolumePrefixSize ||
      volume.partition_count < 2 || volume.partition_size == 0 ||
      !is_power_of_two(volume.persistence_quantum) ||
      volume.persistence_quantum > internal::kPartitionHeaderRegionSize ||
      volume.max_block_payload == 0 ||
      volume.index_region_size < internal::kIndexEntryEncodedSize ||
      internal::kVolumePrefixSize % volume.persistence_quantum != 0 ||
      internal::kPartitionHeaderRegionSize % volume.persistence_quantum != 0 ||
      volume.partition_size % volume.persistence_quantum != 0 ||
      volume.index_region_size % volume.persistence_quantum != 0)
    return Status::Error(StatusCode::corrupt, "invalid volume geometry");
  const std::uint64_t fixed =
      static_cast<std::uint64_t>(internal::kPartitionHeaderRegionSize) +
      internal::kPartitionFooterRegionSize + volume.index_region_size;
  if (fixed >= volume.partition_size ||
      volume.partition_size - fixed < volume.persistence_quantum ||
      volume.partition_count >
          (volume.volume_size - internal::kVolumePrefixSize) /
              volume.partition_size)
    return Status::Error(StatusCode::corrupt, "volume geometry exceeds backend");
  bool overflow = false;
  const std::uint64_t maximum_span = internal::align_up(
      static_cast<std::uint64_t>(internal::kBlockHeaderEncodedSize) +
          volume.max_block_payload,
      volume.persistence_quantum, &overflow);
  if (overflow || maximum_span > volume.partition_size - fixed)
    return Status::Error(StatusCode::corrupt,
                         "maximum block does not fit partition");
  return Status::Ok();
}

bool same_index_entry(const IndexEntry& a, const IndexEntry& b) {
  return a.offset == b.offset && a.frame_size == b.frame_size &&
         a.frame_span == b.frame_span && a.sequence == b.sequence &&
         a.record_count == b.record_count &&
         a.min_time_ns == b.min_time_ns && a.max_time_ns == b.max_time_ns &&
         a.flags == b.flags && a.raw_size == b.raw_size;
}

bool ranges_intersect(std::int64_t min_time, std::int64_t max_time,
                      const TimeRange& range) {
  // Stored block ranges are closed; queries are half-open.
  return min_time < range.end_ns && max_time >= range.begin_ns;
}

bool delta_exceeds(std::int64_t earlier, std::int64_t later,
                   std::int64_t allowed) {
  if (allowed < 0) return false;
  if (later < earlier) return true;
  const std::uint64_t distance = static_cast<std::uint64_t>(later) -
                                 static_cast<std::uint64_t>(earlier);
  return distance > static_cast<std::uint64_t>(allowed);
}

std::string slot_message(const char* prefix, std::uint32_t slot,
                         std::uint64_t generation) {
  std::ostringstream out;
  out << prefix << " slot=" << slot << " generation=" << generation;
  return out.str();
}

Status random_nonzero_id(std::uint64_t* high, std::uint64_t* low) {
  if (!high || !low)
    return Status::Error(StatusCode::invalid_argument, "null random ID output");
  std::uint64_t id[2] = {0, 0};
  do {
    std::size_t done = 0;
    while (done != sizeof id) {
      const ssize_t result = ::getrandom(
          reinterpret_cast<std::uint8_t*>(id) + done, sizeof id - done, 0);
      if (result < 0) {
        if (errno == EINTR) continue;
        return Status::Error(StatusCode::io_error,
                             std::string("getrandom: ") + std::strerror(errno));
      }
      if (result == 0)
        return Status::Error(StatusCode::io_error, "getrandom made no progress");
      done += static_cast<std::size_t>(result);
    }
  } while (id[0] == 0 && id[1] == 0);
  *high = id[0];
  *low = id[1];
  return Status::Ok();
}

}  // namespace

struct RingStore::Impl {
  struct PartitionState {
    PartitionHeader header;
    bool sealed = false;
    bool index_valid = false;
    PartitionFooter footer;
    std::vector<IndexEntry> active_index;
    std::uint64_t data_end = internal::kPartitionHeaderRegionSize;
    std::uint32_t aggregate_flags = 0;
    bool has_time = false;
    std::uint32_t block_count = 0;
    std::int64_t min_time_ns = 0;
    std::int64_t max_time_ns = 0;
  };

  std::shared_ptr<Storage> storage;
  VolumeHeader volume;
  OpenOptions open_options;
  bool writable = false;
  bool closed = false;
  bool faulted = false;
  Status fault_status;
  mutable std::mutex mutex;
  std::vector<PartitionState> partitions;
  std::uint64_t active_generation = 0;
  std::uint32_t active_slot = 0;
  std::uint64_t writer_id_high = 0;
  std::uint64_t writer_id_low = 0;
  PartitionOptions next_options;
  std::vector<std::uint8_t> builder;
  std::uint32_t builder_records = 0;
  std::uint32_t builder_flags = 0;
  std::int64_t builder_min_time = 0;
  std::int64_t builder_max_time = 0;
  bool have_last_time = false;
  std::int64_t last_time = 0;
  bool dirty = false;
  bool have_undurable_records = false;
  std::chrono::steady_clock::time_point oldest_undurable_record;
  mutable StoreMetrics metrics;
  std::vector<std::uint32_t> corrupt_slots;

  // Mirrors of the few observations a watchdog needs, published atomically so
  // health() can read them without the mutex that a backend flush is held
  // under. Everything here is written from paths that already hold that
  // mutex, so no extra synchronization is introduced on the hot path.
  std::atomic<bool> health_closed;
  std::atomic<bool> health_faulted;
  std::atomic<int> health_fault_code;
  std::atomic<bool> health_have_undurable;
  std::atomic<std::uint64_t> health_oldest_undurable_ns;
  std::atomic<std::uint64_t> health_last_sync_ns;
  std::atomic<std::uint64_t> health_max_sync_ns;
  std::atomic<std::uint64_t> health_sync_calls;
  std::atomic<std::uint64_t> health_sync_errors;
  std::atomic<std::uint64_t> health_checkpoints;

  Impl()
      : health_closed(false), health_faulted(false),
        health_fault_code(static_cast<int>(StatusCode::ok)),
        health_have_undurable(false), health_oldest_undurable_ns(0),
        health_last_sync_ns(0), health_max_sync_ns(0), health_sync_calls(0),
        health_sync_errors(0), health_checkpoints(0) {}

  static std::uint64_t steady_now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
  }

  std::uint64_t partition_base(std::uint32_t slot) const {
    return internal::kVolumePrefixSize +
           static_cast<std::uint64_t>(slot) * volume.partition_size;
  }
  std::uint64_t index_begin() const {
    return volume.partition_size - internal::kPartitionFooterRegionSize -
           volume.index_region_size;
  }
  std::uint64_t footer_offset() const {
    return volume.partition_size - internal::kPartitionFooterRegionSize;
  }

  PartitionState* active() {
    for (PartitionState& state : partitions)
      if (state.header.generation == active_generation &&
          state.header.slot == active_slot) return &state;
    return nullptr;
  }

  Status fail(const Status& status) {
    if (status.ok()) return status;
    faulted = true;
    fault_status = status;
    health_fault_code.store(static_cast<int>(status.code()),
                            std::memory_order_relaxed);
    // Publish the associated code before faulted becomes observable. Fields
    // that have no cross-field invariant remain relaxed below.
    health_faulted.store(true, std::memory_order_release);
    return status;
  }

  Status sync_media(bool advances_durability) {
    const std::chrono::steady_clock::time_point started =
        std::chrono::steady_clock::now();
    Status status = storage->flush();
    const std::uint64_t elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count());
    ++metrics.sync_calls;
    metrics.last_sync_duration_ns = elapsed;
    metrics.max_sync_duration_ns = std::max(metrics.max_sync_duration_ns,
                                             elapsed);
    health_sync_calls.store(metrics.sync_calls, std::memory_order_relaxed);
    health_last_sync_ns.store(elapsed, std::memory_order_relaxed);
    health_max_sync_ns.store(metrics.max_sync_duration_ns,
                             std::memory_order_relaxed);
    if (!status.ok()) {
      ++metrics.sync_errors;
      health_sync_errors.store(metrics.sync_errors, std::memory_order_relaxed);
      return fail(status);
    }
    if (advances_durability) {
      dirty = false;
      ++metrics.checkpoints;
      metrics.durable_records = metrics.accepted_records;
      metrics.durable_blocks = metrics.published_blocks;
      health_checkpoints.store(metrics.checkpoints, std::memory_order_relaxed);
      if (have_undurable_records) {
        const std::uint64_t age = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() -
                    oldest_undurable_record).count());
        metrics.max_accepted_to_durable_ns =
            std::max(metrics.max_accepted_to_durable_ns, age);
        have_undurable_records = false;
        health_have_undurable.store(false, std::memory_order_relaxed);
      }
    }
    return Status::Ok();
  }

  std::shared_ptr<const CompressionCodec> codec_for(CompressionId id,
                                                     std::uint16_t version) const {
    std::shared_ptr<const CompressionCodec> codec = built_in_codec(id);
    if (codec && codec->version() == version) return codec;
    for (const std::shared_ptr<const CompressionCodec>& candidate :
         open_options.compression_codecs) {
      if (candidate && candidate->id() == id && candidate->version() == version)
        return candidate;
    }
    return std::shared_ptr<const CompressionCodec>();
  }

  Status validate_partition_options(const PartitionOptions& options) const {
    if (!codec_for(options.compression, options.compression_version))
      return Status::Error(StatusCode::unsupported,
                           "compression codec/version unavailable");
    if (options.time_domain_id == 0)
      return Status::Error(StatusCode::invalid_argument,
                           "time domain zero is reserved for physical scans");
    if ((options.record_format_id == 0) !=
        (options.record_format_version == 0))
      return Status::Error(StatusCode::invalid_argument,
                           "record profile ID and version must both be zero or nonzero");
    return Status::Ok();
  }

  Status read_partition_header(std::uint32_t slot, PartitionHeader* header) const {
    std::vector<std::uint8_t> bytes(internal::kPartitionHeaderEncodedSize);
    Status status = read_exact(*storage, partition_base(slot), MutableByteView(bytes.data(), bytes.size()));
    if (!status.ok()) return status;
    status = internal::decode_partition_header(ByteView(bytes), header);
    if (!status.ok()) return status;
    if (header->volume_id_high != volume.volume_id_high ||
        header->volume_id_low != volume.volume_id_low || header->slot != slot ||
        header->max_block_payload != volume.max_block_payload ||
        header->persistence_quantum != volume.persistence_quantum) {
      return Status::Error(StatusCode::corrupt, "partition identity/geometry mismatch");
    }
    Status options_status = validate_partition_options(header->options);
    if (!options_status.ok()) return options_status;
    for (const internal::FeatureDescriptor& feature : header->features) {
      if (feature.region_offset > volume.partition_size ||
          feature.region_length > volume.partition_size - feature.region_offset)
        return Status::Error(StatusCode::corrupt, "feature region outside partition");
      if (feature.kind == internal::kFeatureTimeIndex &&
          (feature.region_offset != index_begin() ||
           feature.region_length != volume.index_region_size))
        return Status::Error(StatusCode::corrupt, "time-index region mismatch");
      if (feature.kind != internal::kFeatureCompression &&
          feature.kind != internal::kFeatureTimeIndex &&
          feature.kind != internal::kFeatureRecordProfile &&
          feature.kind != internal::kFeatureIntegrity &&
          feature.region_length != 0)
        return Status::Error(StatusCode::unsupported,
                             "unknown feature reserves media space");
    }
    return Status::Ok();
  }

  Status read_footer(const PartitionHeader& header, PartitionFooter* footer) const {
    std::vector<std::uint8_t> bytes(internal::kPartitionFooterEncodedSize);
    Status status = read_exact(*storage, partition_base(header.slot) + footer_offset(),
                               MutableByteView(bytes.data(), bytes.size()));
    if (!status.ok()) return status;
    status = internal::decode_partition_footer(ByteView(bytes), footer);
    if (!status.ok()) return status;
    if (footer->volume_id_high != volume.volume_id_high ||
        footer->volume_id_low != volume.volume_id_low ||
        footer->slot != header.slot || footer->generation != header.generation ||
        footer->index_offset != index_begin() ||
        footer->index_size > volume.index_region_size ||
        footer->data_end < internal::kPartitionHeaderRegionSize ||
        footer->data_end > index_begin() ||
        footer->data_end % volume.persistence_quantum != 0 ||
        footer->index_size != static_cast<std::uint64_t>(footer->block_count) *
                                  internal::kIndexEntryEncodedSize ||
        (footer->block_count == 0 &&
         footer->data_end != internal::kPartitionHeaderRegionSize) ||
        (footer->block_count != 0 && footer->min_time_ns > footer->max_time_ns)) {
      return Status::Error(StatusCode::corrupt, "footer identity/bounds mismatch");
    }
    return Status::Ok();
  }

  Status scan_partition(const PartitionHeader& header,
                        std::vector<IndexEntry>* entries,
                        std::uint64_t* data_end,
                        bool* stopped_on_corruption,
                        std::uint64_t scan_limit = 0) const {
    entries->clear();
    *data_end = internal::kPartitionHeaderRegionSize;
    if (stopped_on_corruption) *stopped_on_corruption = false;
    const std::uint64_t limit = scan_limit == 0 ? index_begin() : scan_limit;
    if (limit < internal::kPartitionHeaderRegionSize || limit > index_begin())
      return Status::Error(StatusCode::corrupt, "partition scan limit outside data region");
    std::uint64_t cursor = internal::kPartitionHeaderRegionSize;
    std::uint32_t expected_sequence = 0;
    std::uint64_t expected_writer_high = 0;
    std::uint64_t expected_writer_low = 0;
    while (cursor + internal::kBlockHeaderEncodedSize <= limit) {
      std::vector<std::uint8_t> header_bytes(internal::kBlockHeaderEncodedSize);
      Status status = read_exact(*storage, partition_base(header.slot) + cursor,
                                 MutableByteView(header_bytes.data(), header_bytes.size()));
      if (!status.ok()) return status;
      BlockHeader block;
      status = internal::decode_block_header(ByteView(header_bytes), &block);
      if (!status.ok()) {
        if (stopped_on_corruption) *stopped_on_corruption = true;
        break;
      }
      if (block.generation != header.generation || block.slot != header.slot ||
          block.volume_id_low != volume.volume_id_low ||
          block.volume_id_high != volume.volume_id_high ||
          block.previous_writer_id_high != expected_writer_high ||
          block.previous_writer_id_low != expected_writer_low ||
          block.sequence != expected_sequence ||
          block.record_count == 0 || block.min_time_ns > block.max_time_ns ||
          block.stored_size > volume.max_block_payload ||
          block.raw_size > volume.max_block_payload ||
          (block.compression != CompressionId::none &&
           (block.compression != header.options.compression ||
            block.compression_version != header.options.compression_version)) ||
          (block.compression == CompressionId::none &&
           (block.stored_size != block.raw_size || block.compression_version != 1)) ||
          block.frame_span % volume.persistence_quantum != 0 ||
          cursor % volume.persistence_quantum != 0 ||
          block.frame_span > limit - cursor) {
        if (stopped_on_corruption) *stopped_on_corruption = true;
        break;
      }
      bool span_overflow = false;
      const std::uint64_t expected_span = internal::align_up(
          static_cast<std::uint64_t>(internal::kBlockHeaderEncodedSize) +
              block.stored_size,
          volume.persistence_quantum, &span_overflow);
      if (span_overflow || block.frame_span != expected_span) {
        if (stopped_on_corruption) *stopped_on_corruption = true;
        break;
      }
      std::vector<std::uint8_t> payload(block.stored_size);
      status = read_exact(*storage,
                          partition_base(header.slot) + cursor + internal::kBlockHeaderEncodedSize,
                          MutableByteView(payload.data(), payload.size()));
      if (!status.ok()) return status;
      if (internal::crc32c(ByteView(payload)) != block.payload_crc) {
        if (stopped_on_corruption) *stopped_on_corruption = true;
        break;
      }
      IndexEntry entry;
      entry.offset = cursor;
      entry.frame_size = internal::kBlockHeaderEncodedSize + block.stored_size;
      entry.frame_span = block.frame_span;
      entry.sequence = block.sequence;
      entry.record_count = block.record_count;
      entry.min_time_ns = block.min_time_ns;
      entry.max_time_ns = block.max_time_ns;
      entry.flags = block.flags;
      entry.raw_size = block.raw_size;
      entry.writer_id_high = block.writer_id_high;
      entry.writer_id_low = block.writer_id_low;
      entries->push_back(entry);
      expected_writer_high = block.writer_id_high;
      expected_writer_low = block.writer_id_low;
      cursor += block.frame_span;
      ++expected_sequence;
    }
    *data_end = cursor;
    return Status::Ok();
  }

  Status load_index(const PartitionState& state,
                    std::vector<IndexEntry>* entries) const {
    if (!state.sealed)
      return Status::Error(StatusCode::invalid_argument, "partition is not sealed");
    if (state.footer.index_size == 0) {
      entries->clear();
      return Status::Ok();
    }
    std::vector<std::uint8_t> bytes(state.footer.index_size);
    Status status = read_exact(*storage,
        partition_base(state.header.slot) + state.footer.index_offset,
        MutableByteView(bytes.data(), bytes.size()));
    if (!status.ok()) return status;
    if (internal::crc32c(ByteView(bytes)) != state.footer.index_crc)
      return Status::Error(StatusCode::corrupt, "partition index CRC mismatch");
    status = internal::decode_index(ByteView(bytes), entries);
    if (!status.ok()) return status;
    if (entries->size() != state.footer.block_count)
      return Status::Error(StatusCode::corrupt, "partition index count mismatch");
    std::uint64_t expected_offset = internal::kPartitionHeaderRegionSize;
    for (std::size_t i = 0; i != entries->size(); ++i) {
      const IndexEntry& entry = (*entries)[i];
      if (entry.sequence != i || entry.offset != expected_offset ||
          entry.record_count == 0 || entry.min_time_ns > entry.max_time_ns ||
          entry.frame_size < internal::kBlockHeaderEncodedSize ||
          entry.frame_size > internal::kBlockHeaderEncodedSize +
                                 volume.max_block_payload ||
          entry.raw_size > volume.max_block_payload ||
          entry.frame_span % volume.persistence_quantum != 0 ||
          entry.frame_span > index_begin() - entry.offset) {
        return Status::Error(StatusCode::corrupt, "partition index sequence/bounds mismatch");
      }
      bool span_overflow = false;
      const std::uint64_t expected_span = internal::align_up(
          entry.frame_size, volume.persistence_quantum, &span_overflow);
      if (span_overflow || entry.frame_span != expected_span)
        return Status::Error(StatusCode::corrupt,
                             "partition index frame span mismatch");
      expected_offset += entry.frame_span;
    }
    if (expected_offset != state.footer.data_end)
      return Status::Error(StatusCode::corrupt, "partition index data end mismatch");
    return Status::Ok();
  }

  void summarize(PartitionState* state) {
    state->aggregate_flags = 0;
    state->has_time = false;
    const std::vector<IndexEntry>& entries = state->active_index;
    state->block_count = static_cast<std::uint32_t>(entries.size());
    for (const IndexEntry& entry : entries) {
      state->aggregate_flags |= entry.flags;
      if (!state->has_time) {
        state->has_time = true;
        state->min_time_ns = entry.min_time_ns;
        state->max_time_ns = entry.max_time_ns;
      } else {
        state->min_time_ns = std::min(state->min_time_ns, entry.min_time_ns);
        state->max_time_ns = std::max(state->max_time_ns, entry.max_time_ns);
      }
    }
  }

  Status create_partition(std::uint32_t slot, std::uint64_t generation,
                          const PartitionOptions& options) {
    if (generation == 0)
      return Status::Error(StatusCode::generation_exhausted, "partition generation exhausted");
    const PartitionState* previous = active();
    const bool time_domain_changed = previous &&
        previous->header.options.time_domain_id != options.time_domain_id;
    PartitionHeader header;
    header.volume_id_high = volume.volume_id_high;
    header.volume_id_low = volume.volume_id_low;
    header.generation = generation;
    header.slot = slot;
    header.created_time_ns = 0;
    header.max_block_payload = volume.max_block_payload;
    header.persistence_quantum = volume.persistence_quantum;
    header.options = options;
    Status options_status = validate_partition_options(options);
    if (!options_status.ok()) return options_status;
    std::vector<std::uint8_t> bytes = internal::encode_partition_header(
        header, volume.partition_size, volume.index_region_size);
    Status status = write_exact(*storage, partition_base(slot), ByteView(bytes));
    if (!status.ok()) return fail(status);
    status = sync_media(false);
    if (!status.ok()) return status;

    partitions.erase(std::remove_if(partitions.begin(), partitions.end(),
        [slot](const PartitionState& value) { return value.header.slot == slot; }),
        partitions.end());
    corrupt_slots.erase(std::remove(corrupt_slots.begin(), corrupt_slots.end(),
                                    slot), corrupt_slots.end());
    PartitionState state;
    state.header = header;
    state.data_end = internal::kPartitionHeaderRegionSize;
    partitions.push_back(state);
    std::sort(partitions.begin(), partitions.end(),
        [](const PartitionState& a, const PartitionState& b) {
          return a.header.generation < b.header.generation;
        });
    active_generation = generation;
    active_slot = slot;
    if (time_domain_changed) have_last_time = false;
    dirty = false;
    return Status::Ok();
  }

  Status seal_active_only() {
    PartitionState* state = active();
    if (!state || state->sealed)
      return Status::Error(StatusCode::internal_error, "no active partition to seal");
    // The first flush is required: footer must not become durable before data.
    if (dirty) {
      Status status = sync_media(true);
      if (!status.ok()) return status;
    }
    std::vector<std::uint8_t> index_bytes = internal::encode_index(state->active_index);
    if (index_bytes.size() > volume.index_region_size)
      return fail(Status::Error(StatusCode::internal_error, "active index exceeds reserve"));
    if (!index_bytes.empty()) {
      Status status = write_exact(*storage,
          partition_base(state->header.slot) + index_begin(), ByteView(index_bytes));
      if (!status.ok()) return fail(status);
    }
    PartitionFooter footer;
    footer.volume_id_high = volume.volume_id_high;
    footer.volume_id_low = volume.volume_id_low;
    footer.generation = state->header.generation;
    footer.slot = state->header.slot;
    footer.flags = state->aggregate_flags;
    footer.block_count = static_cast<std::uint32_t>(state->active_index.size());
    footer.index_offset = index_begin();
    footer.index_size = static_cast<std::uint32_t>(index_bytes.size());
    footer.data_end = state->data_end;
    footer.min_time_ns = state->has_time ? state->min_time_ns : 0;
    footer.max_time_ns = state->has_time ? state->max_time_ns : 0;
    footer.index_crc = internal::crc32c(ByteView(index_bytes));
    std::vector<std::uint8_t> footer_bytes = internal::encode_partition_footer(footer);
    Status status = write_exact(*storage,
        partition_base(state->header.slot) + footer_offset(), ByteView(footer_bytes));
    if (!status.ok()) return fail(status);
    status = sync_media(false);
    if (!status.ok()) return status;
    state->sealed = true;
    state->index_valid = true;
    state->footer = footer;
    return Status::Ok();
  }

  Status rotate_internal(const PartitionOptions& options) {
    PartitionState* state = active();
    if (!state) return Status::Error(StatusCode::internal_error, "missing active partition");
    if (state->header.generation == std::numeric_limits<std::uint64_t>::max())
      return fail(Status::Error(StatusCode::generation_exhausted, "partition generation exhausted"));
    Status status = seal_active_only();
    if (!status.ok()) return status;
    const std::uint32_t next_slot = (state->header.slot + 1u) % volume.partition_count;
    const std::uint64_t next_generation = state->header.generation + 1u;
    status = create_partition(next_slot, next_generation, options);
    if (status.ok()) ++metrics.rotations;
    return status;
  }

  Status ensure_empty_builder_capacity() {
    if (!builder.empty()) return Status::Ok();
    PartitionState* state = active();
    if (!state) return Status::Error(StatusCode::internal_error, "missing active partition");
    bool overflow = false;
    const std::uint64_t worst_span = internal::align_up(
        static_cast<std::uint64_t>(internal::kBlockHeaderEncodedSize) +
            volume.max_block_payload,
        volume.persistence_quantum, &overflow);
    if (overflow || worst_span > index_begin() - internal::kPartitionHeaderRegionSize)
      return fail(Status::Error(StatusCode::no_space,
                                "configured block cannot fit empty partition"));
    const bool data_full = state->data_end > index_begin() ||
                           worst_span > index_begin() - state->data_end;
    const bool index_full = state->active_index.size() + 1 >
                            volume.index_region_size /
                                internal::kIndexEntryEncodedSize;
    return (data_full || index_full) ? rotate_internal(next_options) : Status::Ok();
  }

  Status emit_builder() {
    if (builder.empty()) return Status::Ok();
    PartitionState* state = active();
    if (!state) return fail(Status::Error(StatusCode::internal_error, "missing active partition"));
    std::shared_ptr<const CompressionCodec> configured = codec_for(
        state->header.options.compression,
        state->header.options.compression_version);
    if (!configured) return fail(Status::Error(StatusCode::unsupported, "compression codec unavailable"));

    std::vector<std::uint8_t> compressed;
    Status status;
    CompressionId selected = CompressionId::none;
    std::uint16_t selected_version = 1;
    ByteView payload(builder);
    if (state->header.options.compression != CompressionId::none) {
      status = configured->compress(ByteView(builder), &compressed);
      if (!status.ok()) return fail(status);
      if (compressed.size() > configured->max_compressed_size(builder.size()))
        return fail(Status::Error(StatusCode::internal_error,
                                  "compression codec exceeded declared bound"));
      if (compressed.empty())
        return fail(Status::Error(StatusCode::internal_error,
                                  "compression codec returned empty output"));
    }
    if (state->header.options.compression != CompressionId::none &&
        compressed.size() < builder.size()) {
      selected = state->header.options.compression;
      selected_version = configured->version();
      payload = ByteView(compressed);
    }
    const std::uint64_t frame_size64 = internal::kBlockHeaderEncodedSize + payload.size();
    bool overflow = false;
    const std::uint64_t span64 = internal::align_up(frame_size64,
                                                    volume.persistence_quantum,
                                                    &overflow);
    if (overflow || frame_size64 > UINT32_MAX || span64 > UINT32_MAX)
      return fail(Status::Error(StatusCode::out_of_range, "encoded block too large"));

    const std::size_t prospective_entries = state->active_index.size() + 1;
    if (state->data_end > index_begin() || span64 > index_begin() - state->data_end ||
        prospective_entries > volume.index_region_size / internal::kIndexEntryEncodedSize)
      return fail(Status::Error(StatusCode::internal_error,
          "builder capacity was not reserved before accepting records"));

    BlockHeader block;
    block.generation = state->header.generation;
    block.slot = state->header.slot;
    block.sequence = static_cast<std::uint32_t>(state->active_index.size());
    block.flags = builder_flags;
    block.record_count = builder_records;
    block.stored_size = static_cast<std::uint32_t>(payload.size());
    block.raw_size = static_cast<std::uint32_t>(builder.size());
    block.frame_span = static_cast<std::uint32_t>(span64);
    block.min_time_ns = builder_min_time;
    block.max_time_ns = builder_max_time;
    block.compression = selected;
    block.compression_version = selected_version;
    block.payload_crc = internal::crc32c(payload);
    block.volume_id_low = volume.volume_id_low;
    block.volume_id_high = volume.volume_id_high;
    block.writer_id_high = writer_id_high;
    block.writer_id_low = writer_id_low;
    if (!state->active_index.empty()) {
      block.previous_writer_id_high = state->active_index.back().writer_id_high;
      block.previous_writer_id_low = state->active_index.back().writer_id_low;
    }
    std::vector<std::uint8_t> header_bytes = internal::encode_block_header(block);
    std::vector<std::uint8_t> frame;
    frame.reserve(static_cast<std::size_t>(frame_size64));
    frame.insert(frame.end(), header_bytes.begin(), header_bytes.end());
    frame.insert(frame.end(), payload.data(), payload.data() + payload.size());
    status = write_exact(*storage, partition_base(state->header.slot) + state->data_end,
                         ByteView(frame));
    if (!status.ok()) return fail(status);

    IndexEntry entry;
    entry.offset = state->data_end;
    entry.frame_size = static_cast<std::uint32_t>(frame_size64);
    entry.frame_span = static_cast<std::uint32_t>(span64);
    entry.sequence = block.sequence;
    entry.record_count = block.record_count;
    entry.min_time_ns = block.min_time_ns;
    entry.max_time_ns = block.max_time_ns;
    entry.flags = block.flags;
    entry.raw_size = block.raw_size;
    entry.writer_id_high = block.writer_id_high;
    entry.writer_id_low = block.writer_id_low;
    state->active_index.push_back(entry);
    state->block_count = static_cast<std::uint32_t>(state->active_index.size());
    state->data_end += span64;
    state->aggregate_flags |= block.flags;
    if (!state->has_time) {
      state->has_time = true;
      state->min_time_ns = block.min_time_ns;
      state->max_time_ns = block.max_time_ns;
    } else {
      state->min_time_ns = std::min(state->min_time_ns, block.min_time_ns);
      state->max_time_ns = std::max(state->max_time_ns, block.max_time_ns);
    }
    ++metrics.published_blocks;
    metrics.stored_block_bytes += frame.size();
    metrics.raw_block_bytes += builder.size();
    dirty = true;
    builder.clear();
    builder_records = 0;
    builder_flags = 0;
    return Status::Ok();
  }

  Status classify_snapshot_failure(const PartitionState& partition,
                                   const Status& fallback) const {
    PartitionHeader current_header;
    const Status status = read_partition_header(partition.header.slot,
                                                &current_header);
    if (status.ok() &&
        current_header.generation != partition.header.generation)
      return Status::Error(StatusCode::overwritten,
                           "partition changed while reading block");
    if (!status.ok() &&
        (status.code() == StatusCode::io_error ||
         status.code() == StatusCode::interrupted ||
         status.code() == StatusCode::unsupported))
      return status;
    return fallback;
  }

  Status read_block(const PartitionState& partition, const IndexEntry& entry,
                    BlockMetadata* metadata,
                    std::vector<std::uint8_t>* decoded) const {
    PartitionHeader current_header;
    Status status = read_partition_header(partition.header.slot, &current_header);
    if (!status.ok()) return classify_snapshot_failure(partition, status);
    if (current_header.generation != partition.header.generation) {
      return Status::Error(StatusCode::overwritten,
          slot_message("partition was overwritten", partition.header.slot,
                       partition.header.generation));
    }
    std::vector<std::uint8_t> header_bytes(internal::kBlockHeaderEncodedSize);
    status = read_exact(*storage, partition_base(partition.header.slot) + entry.offset,
                        MutableByteView(header_bytes.data(), header_bytes.size()));
    if (!status.ok()) return status;
    BlockHeader block;
    status = internal::decode_block_header(ByteView(header_bytes), &block);
    if (!status.ok()) return classify_snapshot_failure(partition, status);
    if (block.generation != partition.header.generation ||
        block.slot != partition.header.slot || block.sequence != entry.sequence ||
          block.volume_id_low != volume.volume_id_low ||
          block.volume_id_high != volume.volume_id_high ||
          ((entry.writer_id_high != 0 || entry.writer_id_low != 0) &&
              (block.writer_id_high != entry.writer_id_high ||
               block.writer_id_low != entry.writer_id_low))) {
      return classify_snapshot_failure(
          partition, Status::Error(StatusCode::corrupt,
                                   "block identity disagrees with snapshot"));
    }
    if (block.stored_size + internal::kBlockHeaderEncodedSize != entry.frame_size ||
          block.frame_span != entry.frame_span || block.raw_size > volume.max_block_payload ||
          block.stored_size > volume.max_block_payload ||
          block.record_count == 0 || block.min_time_ns > block.max_time_ns ||
          block.record_count != entry.record_count || block.raw_size != entry.raw_size ||
          block.min_time_ns != entry.min_time_ns || block.max_time_ns != entry.max_time_ns ||
          block.flags != entry.flags ||
          (block.compression != CompressionId::none &&
           (block.compression != partition.header.options.compression ||
            block.compression_version !=
                partition.header.options.compression_version)) ||
          (block.compression == CompressionId::none &&
           (block.stored_size != block.raw_size ||
            block.compression_version != 1))) {
      return classify_snapshot_failure(
          partition, Status::Error(StatusCode::corrupt,
                                   "block metadata disagrees with partition/index"));
    }
    bool span_overflow = false;
    if (block.frame_span != internal::align_up(
            static_cast<std::uint64_t>(internal::kBlockHeaderEncodedSize) +
                block.stored_size,
            volume.persistence_quantum, &span_overflow) || span_overflow)
      return classify_snapshot_failure(
          partition, Status::Error(StatusCode::corrupt,
                                   "block frame span mismatch"));
    std::vector<std::uint8_t> stored(block.stored_size);
    status = read_exact(*storage,
        partition_base(partition.header.slot) + entry.offset + internal::kBlockHeaderEncodedSize,
        MutableByteView(stored.data(), stored.size()));
    if (!status.ok()) return status;
    PartitionHeader after_read;
    status = read_partition_header(partition.header.slot, &after_read);
    if (!status.ok()) return classify_snapshot_failure(partition, status);
    if (after_read.generation != partition.header.generation) {
      return Status::Error(StatusCode::overwritten,
                           "partition changed while reading block payload");
    }
    if (internal::crc32c(ByteView(stored)) != block.payload_crc)
      return classify_snapshot_failure(
          partition, Status::Error(StatusCode::corrupt,
                                   "block payload CRC mismatch"));
    std::shared_ptr<const CompressionCodec> codec = codec_for(block.compression,
                                                              block.compression_version);
    if (!codec)
      return Status::Error(StatusCode::unsupported, "block compression unavailable");
    status = codec->decompress(ByteView(stored), block.raw_size, decoded);
    if (!status.ok()) return status;
    if (decoded->size() != block.raw_size)
      return Status::Error(StatusCode::corrupt,
                           "compression codec returned wrong output size");
    metadata->partition_slot = partition.header.slot;
    metadata->partition_generation = partition.header.generation;
    metadata->block_sequence = block.sequence;
    metadata->record_count = block.record_count;
    metadata->min_time_ns = block.min_time_ns;
    metadata->max_time_ns = block.max_time_ns;
    metadata->flags = block.flags;
    metadata->compression = block.compression;
    metadata->stored_size = block.stored_size;
    metadata->raw_size = block.raw_size;
    metadata->physical_offset = partition_base(partition.header.slot) + entry.offset;
    metadata->record_format_id = partition.header.options.record_format_id;
    metadata->record_format_version = partition.header.options.record_format_version;
    metadata->time_domain_id = partition.header.options.time_domain_id;
    return Status::Ok();
  }

  Status query(bool all_blocks, const TimeRange& range,
               const QueryOptions& options, const BlockVisitor& visitor) const {
    if (!visitor) return Status::Error(StatusCode::invalid_argument, "empty block visitor");
    if (!all_blocks && range.end_ns <= range.begin_ns)
      return Status::Error(StatusCode::invalid_argument, "time range must be non-empty [begin,end)");
    if (!all_blocks && range.time_domain_id == 0)
      return Status::Error(StatusCode::invalid_argument,
                           "time domain zero requires scan_blocks()");
    std::vector<PartitionState> snapshot;
    std::vector<std::uint32_t> corrupt_slot_snapshot;
    {
      std::lock_guard<std::mutex> lock(mutex);
      snapshot = partitions;
      corrupt_slot_snapshot = corrupt_slots;
    }
    std::sort(snapshot.begin(), snapshot.end(),
        [](const PartitionState& a, const PartitionState& b) {
          return a.header.generation < b.header.generation;
        });
    for (std::uint32_t slot : corrupt_slot_snapshot) {
      BlockEvent event;
      event.kind = BlockEventKind::corrupt_gap;
      event.metadata.partition_slot = slot;
      event.detail = Status::Error(StatusCode::corrupt,
                                   "partition header is damaged");
      {
        std::lock_guard<std::mutex> lock(mutex);
        ++metrics.corrupt_gaps;
      }
      if (!visitor(event)) return Status::Ok();
      if (!options.continue_on_gap) return event.detail;
    }
    for (const PartitionState& partition : snapshot) {
      if (!all_blocks && range.time_domain_id != 0 &&
          partition.header.options.time_domain_id != range.time_domain_id)
        continue;
      std::vector<IndexEntry> entries;
      const std::vector<IndexEntry>* selected_entries = &entries;
      Status index_status;
      bool corrupt_scan = false;
      bool invalid_sealed_index = false;
      bool rescanned_unsealed = false;
      std::uint64_t scanned_end = 0;
      if (partition.sealed) index_status = load_index(partition, &entries);
      else if (!partition.active_index.empty() || partition.data_end == internal::kPartitionHeaderRegionSize) {
        selected_entries = &partition.active_index;
        index_status = Status::Ok();
      } else {
        rescanned_unsealed = true;
        index_status = scan_partition(partition.header, &entries, &scanned_end,
                                      &corrupt_scan, partition.data_end);
      }
      if (!index_status.ok()) {
        if (index_status.code() != StatusCode::corrupt) return index_status;
        invalid_sealed_index = partition.sealed;
        index_status = scan_partition(
            partition.header, &entries, &scanned_end, &corrupt_scan,
            partition.sealed ? partition.footer.data_end : 0);
      }
      if (!index_status.ok()) return index_status;
      // Index acquisition is deliberately outside the writer mutex. Recheck
      // the slot before interpreting an index/scan failure: rotation may have
      // replaced the entire snapshotted generation while those bytes were
      // being read. That is an overwrite gap, not media corruption.
      PartitionHeader current_header;
      Status generation_status = read_partition_header(partition.header.slot,
                                                        &current_header);
      if (!generation_status.ok() ||
          current_header.generation != partition.header.generation) {
        if (!generation_status.ok() &&
            (generation_status.code() == StatusCode::io_error ||
             generation_status.code() == StatusCode::interrupted ||
             generation_status.code() == StatusCode::unsupported))
          return generation_status;
        BlockEvent event;
        event.kind = generation_status.ok()
            ? BlockEventKind::overwritten_gap : BlockEventKind::corrupt_gap;
        event.detail = generation_status.ok()
            ? Status::Error(StatusCode::overwritten,
                            "partition changed while acquiring its index")
            : generation_status;
        event.metadata.partition_slot = partition.header.slot;
        event.metadata.partition_generation = partition.header.generation;
        {
          std::lock_guard<std::mutex> lock(mutex);
          if (event.kind == BlockEventKind::overwritten_gap)
            ++metrics.overwritten_gaps;
          else
            ++metrics.corrupt_gaps;
        }
        if (!visitor(event)) return Status::Ok();
        if (!options.continue_on_gap) return event.detail;
        continue;
      }
      const bool invalid_sealed_prefix = partition.sealed &&
          (invalid_sealed_index || corrupt_scan ||
           selected_entries->size() != partition.footer.block_count ||
           (scanned_end != 0 && scanned_end != partition.footer.data_end));
      const bool invalid_unsealed_prefix = rescanned_unsealed &&
          (corrupt_scan || scanned_end != partition.data_end ||
           selected_entries->size() != partition.block_count);
      if (invalid_sealed_prefix || invalid_unsealed_prefix) {
        BlockEvent event;
        event.kind = BlockEventKind::corrupt_gap;
        event.detail = Status::Error(
            StatusCode::corrupt,
            partition.sealed ? "sealed partition has invalid data/index prefix"
                             : "footerless partition changed after catalog snapshot");
        event.metadata.partition_slot = partition.header.slot;
        event.metadata.partition_generation = partition.header.generation;
        {
          std::lock_guard<std::mutex> lock(mutex);
          ++metrics.corrupt_gaps;
        }
        if (!visitor(event)) return Status::Ok();
        if (!options.continue_on_gap) return event.detail;
      }
      for (const IndexEntry& entry : *selected_entries) {
        if (!all_blocks && !ranges_intersect(entry.min_time_ns, entry.max_time_ns, range))
          continue;
        BlockMetadata metadata;
        std::vector<std::uint8_t> decoded;
        Status status = read_block(partition, entry, &metadata, &decoded);
        if (!status.ok()) {
          BlockEvent event;
          event.kind = status.code() == StatusCode::overwritten
              ? BlockEventKind::overwritten_gap : BlockEventKind::corrupt_gap;
          event.detail = status;
          event.metadata.partition_slot = partition.header.slot;
          event.metadata.partition_generation = partition.header.generation;
          event.metadata.block_sequence = entry.sequence;
          {
            std::lock_guard<std::mutex> lock(mutex);
            if (event.kind == BlockEventKind::overwritten_gap) ++metrics.overwritten_gaps;
            else ++metrics.corrupt_gaps;
          }
          if (!visitor(event)) return Status::Ok();
          if (!options.continue_on_gap) return status;
          // Exactly one overwrite event for this snapshotted partition.
          if (event.kind == BlockEventKind::overwritten_gap) break;
          continue;
        }
        BlockEvent event;
        event.kind = BlockEventKind::data;
        event.metadata = metadata;
        event.data = ByteView(decoded);
        if (!visitor(event)) return Status::Ok();
      }
    }
    return Status::Ok();
  }
};

RingStore::RingStore(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)), writable_(impl_->writable) {}

RingStore::~RingStore() {}

Status RingStore::format(Storage& storage, const VolumeOptions& options) {
  if (!storage.writable())
    return Status::Error(StatusCode::invalid_argument, "format requires writable storage");
  if (storage.size() < internal::kVolumePrefixSize || options.partition_size == 0 ||
      !is_power_of_two(options.persistence_quantum) ||
      options.persistence_quantum > internal::kPartitionHeaderRegionSize ||
      options.max_block_payload == 0 ||
      options.index_region_size < internal::kIndexEntryEncodedSize ||
      internal::kVolumePrefixSize % options.persistence_quantum != 0 ||
      internal::kPartitionHeaderRegionSize % options.persistence_quantum != 0 ||
      options.partition_size % options.persistence_quantum != 0 ||
      options.index_region_size % options.persistence_quantum != 0 ||
      options.partition_size < static_cast<std::uint64_t>(internal::kPartitionHeaderRegionSize) +
          internal::kPartitionFooterRegionSize + options.index_region_size +
          options.persistence_quantum) {
    return Status::Error(StatusCode::invalid_argument, "invalid volume geometry");
  }
  const std::uint64_t available = storage.size() - internal::kVolumePrefixSize;
  const std::uint64_t count64 = available / options.partition_size;
  if (count64 < 2 || count64 > UINT32_MAX)
    return Status::Error(StatusCode::invalid_argument, "volume needs 2..UINT32_MAX partitions");
  bool frame_overflow = false;
  const std::uint64_t maximum_frame_span = internal::align_up(
      static_cast<std::uint64_t>(internal::kBlockHeaderEncodedSize) +
          options.max_block_payload,
      options.persistence_quantum, &frame_overflow);
  if (frame_overflow || maximum_frame_span >
      options.partition_size - internal::kPartitionHeaderRegionSize -
      internal::kPartitionFooterRegionSize - options.index_region_size) {
    return Status::Error(StatusCode::invalid_argument, "maximum block cannot fit partition");
  }
  VolumeHeader header;
  header.volume_size = storage.size();
  header.partition_size = options.partition_size;
  header.partition_count = static_cast<std::uint32_t>(count64);
  header.index_region_size = options.index_region_size;
  header.max_block_payload = options.max_block_payload;
  header.persistence_quantum = options.persistence_quantum;
  header.created_time_ns = options.created_time_ns;
  if (options.volume_id_high != 0 || options.volume_id_low != 0) {
    if (!options.allow_explicit_volume_id_for_testing)
      return Status::Error(StatusCode::invalid_argument,
                           "explicit volume ID is restricted to tests/golden vectors");
    header.volume_id_high = options.volume_id_high;
    header.volume_id_low = options.volume_id_low;
  } else {
    Status random_status = random_nonzero_id(&header.volume_id_high,
                                             &header.volume_id_low);
    if (!random_status.ok()) return random_status;
  }
  std::vector<std::uint8_t> bytes = internal::encode_volume_header(header);
  Status status = write_exact(storage, 0, ByteView(bytes));
  if (!status.ok()) return status;
  status = write_exact(storage, internal::kVolumeHeaderCopySize, ByteView(bytes));
  if (!status.ok()) return status;
  return storage.flush();
}

Status RingStore::open(std::shared_ptr<Storage> storage,
                       const OpenOptions& options,
                       std::unique_ptr<RingStore>* output) {
  if (!storage || !output)
    return Status::Error(StatusCode::invalid_argument, "null storage/output");
  if (options.writable && !storage->writable())
    return Status::Error(StatusCode::invalid_argument, "writable open on read-only backend");
  std::vector<std::uint8_t> first(internal::kVolumeHeaderEncodedSize);
  std::vector<std::uint8_t> second(internal::kVolumeHeaderEncodedSize);
  Status first_io = read_exact(*storage, 0, MutableByteView(first.data(), first.size()));
  Status second_io = read_exact(*storage, internal::kVolumeHeaderCopySize,
                                MutableByteView(second.data(), second.size()));
  if (!first_io.ok() && !second_io.ok()) return first_io;
  VolumeHeader first_header, second_header;
  Status first_status = first_io.ok() ? internal::decode_volume_header(ByteView(first), &first_header) : first_io;
  Status second_status = second_io.ok() ? internal::decode_volume_header(ByteView(second), &second_header) : second_io;
  VolumeHeader volume;
  if (first_status.ok() && second_status.ok()) {
    if (!same_volume_header(first_header, second_header))
      return Status::Error(StatusCode::corrupt, "valid volume header copies disagree");
    volume = first_header;
  } else if (first_status.ok()) volume = first_header;
  else if (second_status.ok()) volume = second_header;
  else {
    if (first_status.code() == StatusCode::unsupported) return first_status;
    if (second_status.code() == StatusCode::unsupported) return second_status;
    if (first_status.code() == StatusCode::io_error ||
        first_status.code() == StatusCode::interrupted) return first_status;
    if (second_status.code() == StatusCode::io_error ||
        second_status.code() == StatusCode::interrupted) return second_status;
    return Status::Error(StatusCode::corrupt, "both volume headers are invalid");
  }
  Status geometry = validate_volume_geometry(volume, storage->size());
  if (!geometry.ok()) return geometry;

  std::unique_ptr<Impl> impl(new Impl());
  impl->storage = storage;
  impl->volume = volume;
  impl->open_options = options;
  impl->writable = options.writable;
  impl->next_options = options.next_partition;
  impl->builder.reserve(volume.max_block_payload);
  if (options.writable) {
    if (options.writer_id_high != 0 || options.writer_id_low != 0) {
      if (!options.allow_explicit_writer_id_for_testing)
        return Status::Error(StatusCode::invalid_argument,
                             "explicit writer ID is restricted to tests");
      impl->writer_id_high = options.writer_id_high;
      impl->writer_id_low = options.writer_id_low;
    } else {
      Status random_status = random_nonzero_id(&impl->writer_id_high,
                                               &impl->writer_id_low);
      if (!random_status.ok()) return random_status;
    }
    Status next_status = impl->validate_partition_options(options.next_partition);
    if (!next_status.ok()) return next_status;
  }
  std::set<std::uint64_t> generations;
  for (std::uint32_t slot = 0; slot != volume.partition_count; ++slot) {
    PartitionHeader header;
    Status header_status = impl->read_partition_header(slot, &header);
    if (!header_status.ok()) {
      if (header_status.code() == StatusCode::io_error ||
          header_status.code() == StatusCode::interrupted) return header_status;
      std::vector<std::uint8_t> raw(internal::kPartitionHeaderEncodedSize);
      Status raw_status = read_exact(*storage, impl->partition_base(slot),
                                     MutableByteView(raw.data(), raw.size()));
      if (!raw_status.ok()) return raw_status;
      const std::uint8_t magic[8] = {'T','F','D','B','P','A','R','1'};
      bool all_zero = true;
      for (std::uint8_t byte : raw) if (byte != 0) { all_zero = false; break; }
      auto read_u64 = [](const std::uint8_t* data) {
        std::uint64_t value = 0;
        for (unsigned i = 0; i != 8; ++i)
          value |= static_cast<std::uint64_t>(data[i]) << (i * 8);
        return value;
      };
      const bool same_magic = std::memcmp(raw.data(), magic, 8) == 0;
      const bool same_id = read_u64(raw.data() + 16) == volume.volume_id_high &&
                           read_u64(raw.data() + 24) == volume.volume_id_low;
      bool current_volume_evidence = !all_zero && same_magic && same_id;
      if (!all_zero && !current_volume_evidence) {
        std::vector<std::uint8_t> footer_bytes(
            internal::kPartitionFooterEncodedSize);
        Status evidence_io = read_exact(
            *storage, impl->partition_base(slot) + impl->footer_offset(),
            MutableByteView(footer_bytes.data(), footer_bytes.size()));
        if (!evidence_io.ok()) return evidence_io;
        PartitionFooter evidence_footer;
        if (internal::decode_partition_footer(ByteView(footer_bytes),
                                               &evidence_footer).ok() &&
            evidence_footer.slot == slot &&
            evidence_footer.volume_id_high == volume.volume_id_high &&
            evidence_footer.volume_id_low == volume.volume_id_low)
          current_volume_evidence = true;
        std::vector<std::uint8_t> block_bytes(
            internal::kBlockHeaderEncodedSize);
        evidence_io = read_exact(
            *storage,
            impl->partition_base(slot) + internal::kPartitionHeaderRegionSize,
            MutableByteView(block_bytes.data(), block_bytes.size()));
        if (!evidence_io.ok()) return evidence_io;
        BlockHeader evidence_block;
        if (internal::decode_block_header(ByteView(block_bytes),
                                          &evidence_block).ok() &&
            evidence_block.slot == slot &&
            evidence_block.volume_id_high == volume.volume_id_high &&
            evidence_block.volume_id_low == volume.volume_id_low)
          current_volume_evidence = true;
      }
      if (current_volume_evidence) {
        if (header_status.code() == StatusCode::unsupported)
          return header_status;
        if (options.verify_payloads_on_open) return header_status;
        impl->corrupt_slots.push_back(slot);
      }
      continue;  // Empty, stale UUID, or interrupted new header.
    }
    if (header.generation == 0)
      return Status::Error(StatusCode::corrupt, "partition generation zero is reserved");
    if (!generations.insert(header.generation).second)
      return Status::Error(StatusCode::corrupt, "duplicate partition generation");
    Impl::PartitionState state;
    state.header = header;
    PartitionFooter footer;
    Status footer_status = impl->read_footer(header, &footer);
    if (footer_status.ok()) {
      state.sealed = true;
      state.footer = footer;
      state.data_end = footer.data_end;
      state.aggregate_flags = footer.flags;
      state.block_count = footer.block_count;
      state.has_time = footer.block_count != 0;
      state.min_time_ns = footer.min_time_ns;
      state.max_time_ns = footer.max_time_ns;
      // Footer is a provisional seal. Index CRC is checked lazily on query.
      if (options.verify_payloads_on_open) {
        std::vector<IndexEntry> indexed;
        Status strict = impl->load_index(state, &indexed);
        if (!strict.ok()) return strict;
        std::vector<IndexEntry> scanned;
        std::uint64_t scanned_end = 0;
        bool corrupt_tail = false;
        strict = impl->scan_partition(header, &scanned, &scanned_end,
                                      &corrupt_tail, footer.data_end);
        if (!strict.ok()) return strict;
        if (scanned_end != footer.data_end ||
            scanned.size() != indexed.size())
          return Status::Error(StatusCode::corrupt,
                               "strict sealed-partition verification failed");
        for (std::size_t i = 0; i != scanned.size(); ++i)
          if (!same_index_entry(scanned[i], indexed[i]))
            return Status::Error(StatusCode::corrupt,
                                 "sealed index disagrees with block scan");
        std::uint32_t scanned_flags = 0;
        std::int64_t scanned_min_time = 0;
        std::int64_t scanned_max_time = 0;
        if (!scanned.empty()) {
          scanned_min_time = scanned.front().min_time_ns;
          scanned_max_time = scanned.front().max_time_ns;
          for (const IndexEntry& entry : scanned) {
            scanned_flags |= entry.flags;
            scanned_min_time = std::min(scanned_min_time, entry.min_time_ns);
            scanned_max_time = std::max(scanned_max_time, entry.max_time_ns);
          }
        }
        if (footer.flags != scanned_flags ||
            footer.min_time_ns != scanned_min_time ||
            footer.max_time_ns != scanned_max_time)
          return Status::Error(StatusCode::corrupt,
                               "footer summary disagrees with block scan");
        // A header scan proves frame boundaries and the CRC of the stored
        // representation. Strict open additionally exercises the configured
        // decoder and verifies the declared raw size, so a syntactically valid
        // but undecodable compressed block cannot remain latent until query.
        for (const IndexEntry& entry : indexed) {
          BlockMetadata metadata;
          std::vector<std::uint8_t> decoded;
          strict = impl->read_block(state, entry, &metadata, &decoded);
          if (!strict.ok()) return strict;
        }
      }
    } else {
      if (footer_status.code() == StatusCode::io_error ||
          footer_status.code() == StatusCode::interrupted ||
          footer_status.code() == StatusCode::unsupported) return footer_status;
      bool corrupt_tail = false;
      Status scan = impl->scan_partition(header, &state.active_index,
                                         &state.data_end, &corrupt_tail);
      if (!scan.ok()) return scan;
      impl->summarize(&state);
      if (options.verify_payloads_on_open) {
        for (const IndexEntry& entry : state.active_index) {
          BlockMetadata metadata;
          std::vector<std::uint8_t> decoded;
          Status strict = impl->read_block(state, entry, &metadata, &decoded);
          if (!strict.ok()) return strict;
        }
      }
    }
    impl->partitions.push_back(std::move(state));
    // Keep recovery memory independent of the number of damaged footers. At
    // most the newest generation seen so far can remain writable; older
    // footerless indexes are reproducibly rescanned from their saved bounds.
    std::uint64_t newest_seen = 0;
    for (const Impl::PartitionState& candidate : impl->partitions)
      newest_seen = std::max(newest_seen, candidate.header.generation);
    for (Impl::PartitionState& candidate : impl->partitions) {
      if (!candidate.sealed && candidate.header.generation != newest_seen) {
        std::vector<IndexEntry>().swap(candidate.active_index);
      }
    }
  }
  std::sort(impl->partitions.begin(), impl->partitions.end(),
      [](const Impl::PartitionState& a, const Impl::PartitionState& b) {
        return a.header.generation < b.header.generation;
      });
  // Historical footerless partitions are queryable by a bounded rescan. Keep
  // only the newest recovered index resident because it is the sole partition
  // that a writer may continue.
  if (impl->partitions.size() > 1) {
    for (std::size_t i = 0; i + 1 < impl->partitions.size(); ++i) {
      if (!impl->partitions[i].sealed) {
        std::vector<IndexEntry>().swap(impl->partitions[i].active_index);
      }
    }
  }
  if (!impl->partitions.empty()) {
    Impl::PartitionState& newest = impl->partitions.back();
    impl->active_generation = newest.header.generation;
    impl->active_slot = newest.header.slot;
  }
  std::unique_ptr<RingStore> store(new RingStore(std::move(impl)));
  if (options.writable) {
    Impl* value = store->impl_.get();
    if (!value->corrupt_slots.empty())
      return Status::Error(
          StatusCode::corrupt,
          "writable open refuses ambiguous damaged partition headers");
    Status status;
    if (value->partitions.empty()) {
      status = value->create_partition(0, 1, options.next_partition);
    } else if (value->partitions.back().sealed) {
      const Impl::PartitionState& newest = value->partitions.back();
      if (newest.header.generation == std::numeric_limits<std::uint64_t>::max())
        return Status::Error(StatusCode::generation_exhausted, "partition generation exhausted");
      status = value->create_partition((newest.header.slot + 1u) % volume.partition_count,
                                       newest.header.generation + 1u,
                                       options.next_partition);
    }
    if (!status.ok()) return status;
  }
  *output = std::move(store);
  return Status::Ok();
}

Status RingStore::append(ByteView record, std::int64_t time,
                         std::uint32_t record_flags) {
  return append_impl(record, time, record_flags, nullptr);
}

Status RingStore::append_checked(ByteView record, std::int64_t time,
                                 const AppendContract& expected,
                                 std::uint32_t record_flags) {
  if (expected.record_format_id == 0 ||
      expected.record_format_version == 0 || expected.time_domain_id == 0)
    return Status::Error(StatusCode::invalid_argument,
                         "checked append requires a nonzero partition contract");
  return append_impl(record, time, record_flags, &expected);
}

Status RingStore::append_impl(ByteView record, std::int64_t time,
                              std::uint32_t record_flags,
                              const AppendContract* expected) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->writable) return Status::Error(StatusCode::io_error, "store is read-only");
  if (impl_->closed) return Status::Error(StatusCode::closed, "store is closed");
  if (impl_->faulted) return impl_->fault_status;
  if (record.empty()) return Status::Error(StatusCode::invalid_argument, "zero-length record is not allowed");
  if (!record.valid()) return Status::Error(StatusCode::invalid_argument,
                                            "null non-empty record");
  if (record.size() > impl_->volume.max_block_payload)
    return Status::Error(StatusCode::out_of_range, "record exceeds block payload limit");
  if (record.size() > impl_->volume.max_block_payload - impl_->builder.size()) {
    Status status = impl_->emit_builder();
    if (!status.ok()) return status;
  }
  Status capacity = impl_->ensure_empty_builder_capacity();
  if (!capacity.ok()) return capacity;
  Impl::PartitionState* active = impl_->active();
  if (!active) return Status::Error(StatusCode::internal_error,
                                    "missing active partition");
  if (expected &&
      (active->header.options.record_format_id != expected->record_format_id ||
       active->header.options.record_format_version !=
           expected->record_format_version ||
       active->header.options.time_domain_id != expected->time_domain_id))
    return Status::Error(StatusCode::invalid_argument,
                         "active partition contract changed before append");

  std::uint32_t flags = record_flags & 0xffff0000u;
  if (record_flags & kRecordFlagUnsynchronizedTime)
    flags |= kBlockFlagUnsynchronizedTime;
  bool anomaly = false;
  if (impl_->have_last_time) {
    if (time < impl_->last_time &&
        delta_exceeds(time, impl_->last_time,
                      active->header.options.allowed_backward_skew_ns)) anomaly = true;
    if (time > impl_->last_time &&
        delta_exceeds(impl_->last_time, time,
                      active->header.options.allowed_forward_step_ns)) anomaly = true;
  }
  if (anomaly) {
    flags |= kBlockFlagTimeAnomaly;
    ++impl_->metrics.time_anomalies;
  }
  if (impl_->builder.empty()) {
    impl_->builder_min_time = time;
    impl_->builder_max_time = time;
    impl_->builder_flags = flags;
  } else {
    impl_->builder_min_time = std::min(impl_->builder_min_time, time);
    impl_->builder_max_time = std::max(impl_->builder_max_time, time);
    impl_->builder_flags |= flags;
  }
  impl_->builder.insert(impl_->builder.end(), record.data(), record.data() + record.size());
  ++impl_->builder_records;
  impl_->last_time = time;
  impl_->have_last_time = true;
  ++impl_->metrics.accepted_records;
  impl_->metrics.accepted_payload_bytes += record.size();
  if (!impl_->have_undurable_records) {
    impl_->have_undurable_records = true;
    impl_->oldest_undurable_record = std::chrono::steady_clock::now();
    impl_->health_oldest_undurable_ns.store(Impl::steady_now_ns(),
                                            std::memory_order_relaxed);
    // A reader that observes true must also observe the timestamp above.
    impl_->health_have_undurable.store(true, std::memory_order_release);
  }
  return Status::Ok();
}

Status RingStore::publish() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->writable) return Status::Error(StatusCode::io_error, "store is read-only");
  if (impl_->closed) return Status::Error(StatusCode::closed, "store is closed");
  if (impl_->faulted) return impl_->fault_status;
  return impl_->emit_builder();
}

Status RingStore::checkpoint() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->writable) return Status::Error(StatusCode::io_error, "store is read-only");
  if (impl_->closed) return Status::Error(StatusCode::closed, "store is closed");
  if (impl_->faulted) return impl_->fault_status;
  Status status = impl_->emit_builder();
  if (!status.ok()) return status;
  if (!impl_->dirty) return Status::Ok();
  return impl_->sync_media(true);
}

Status RingStore::close() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->closed) return Status::Ok();
  if (!impl_->writable) {
    impl_->closed = true;
    impl_->health_closed.store(true, std::memory_order_relaxed);
    return Status::Ok();
  }
  if (impl_->faulted) return impl_->fault_status;
  Status status = impl_->emit_builder();
  if (!status.ok()) return status;
  if (impl_->dirty) {
    status = impl_->sync_media(true);
    if (!status.ok()) return status;
  }
  impl_->closed = true;
  impl_->health_closed.store(true, std::memory_order_relaxed);
  return Status::Ok();
}

Status RingStore::rotate(const PartitionOptions& next) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->writable) return Status::Error(StatusCode::io_error, "store is read-only");
  if (impl_->closed) return Status::Error(StatusCode::closed, "store is closed");
  if (impl_->faulted) return impl_->fault_status;
  Status validation = impl_->validate_partition_options(next);
  if (!validation.ok()) return validation;
  Status status = impl_->emit_builder();
  if (!status.ok()) return status;
  impl_->next_options = next;
  return impl_->rotate_internal(next);
}

Status RingStore::set_next_partition_options(const PartitionOptions& options) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->writable) return Status::Error(StatusCode::io_error, "store is read-only");
  if (impl_->closed) return Status::Error(StatusCode::closed, "store is closed");
  if (impl_->faulted) return impl_->fault_status;
  Status validation = impl_->validate_partition_options(options);
  if (!validation.ok()) return validation;
  impl_->next_options = options;
  return Status::Ok();
}

Status RingStore::query_blocks(const TimeRange& range, const QueryOptions& options,
                               const BlockVisitor& visitor) const {
  return impl_->query(false, range, options, visitor);
}

Status RingStore::scan_blocks(const QueryOptions& options,
                              const BlockVisitor& visitor) const {
  return impl_->query(true, TimeRange(), options, visitor);
}

Status RingStore::inspect(VolumeInfo* output) const {
  if (!output) return Status::Error(StatusCode::invalid_argument, "null volume info");
  std::lock_guard<std::mutex> lock(impl_->mutex);
  output->volume_size = impl_->volume.volume_size;
  output->partition_size = impl_->volume.partition_size;
  output->partition_count = impl_->volume.partition_count;
  output->index_region_size = impl_->volume.index_region_size;
  output->max_block_payload = impl_->volume.max_block_payload;
  output->persistence_quantum = impl_->volume.persistence_quantum;
  output->volume_id_high = impl_->volume.volume_id_high;
  output->volume_id_low = impl_->volume.volume_id_low;
  output->partitions.clear();
  for (const Impl::PartitionState& state : impl_->partitions) {
    PartitionInfo info;
    info.slot = state.header.slot;
    info.generation = state.header.generation;
    info.sealed = state.sealed;
    info.block_count = state.sealed ? state.footer.block_count
                                    : state.block_count;
    info.data_bytes = state.data_end - internal::kPartitionHeaderRegionSize;
    info.min_time_ns = state.has_time ? state.min_time_ns : 0;
    info.max_time_ns = state.has_time ? state.max_time_ns : 0;
    info.flags = state.aggregate_flags;
    info.options = state.header.options;
    output->partitions.push_back(info);
  }
  return Status::Ok();
}

Status RingStore::active_partition_info(PartitionInfo* output) const {
  if (!output) return Status::Error(StatusCode::invalid_argument,
                                    "null active partition info");
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const Impl::PartitionState* state = impl_->active();
  if (!state) return Status::Error(StatusCode::closed,
                                   "store has no active partition");
  output->slot = state->header.slot;
  output->generation = state->header.generation;
  output->sealed = state->sealed;
  output->block_count = state->sealed ? state->footer.block_count
                                      : state->block_count;
  output->data_bytes = state->data_end - internal::kPartitionHeaderRegionSize;
  output->min_time_ns = state->has_time ? state->min_time_ns : 0;
  output->max_time_ns = state->has_time ? state->max_time_ns : 0;
  output->flags = state->aggregate_flags;
  output->options = state->header.options;
  return Status::Ok();
}

StoreMetrics RingStore::metrics() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  StoreMetrics result = impl_->metrics;
  if (impl_->have_undurable_records) {
    result.oldest_undurable_age_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() -
                impl_->oldest_undurable_record).count());
  }
  return result;
}

WriterHealth RingStore::health() const {
  WriterHealth result;
  result.writable = writable_;
  result.closed = impl_->health_closed.load(std::memory_order_relaxed);
  result.faulted = impl_->health_faulted.load(std::memory_order_acquire);
  result.fault_code = static_cast<StatusCode>(
      impl_->health_fault_code.load(std::memory_order_relaxed));
  if (impl_->health_have_undurable.load(std::memory_order_acquire)) {
    const std::uint64_t since =
        impl_->health_oldest_undurable_ns.load(std::memory_order_relaxed);
    const std::uint64_t now = Impl::steady_now_ns();
    result.oldest_undurable_age_ns = now > since ? now - since : 0;
  }
  result.last_sync_duration_ns =
      impl_->health_last_sync_ns.load(std::memory_order_relaxed);
  result.max_sync_duration_ns =
      impl_->health_max_sync_ns.load(std::memory_order_relaxed);
  result.sync_calls = impl_->health_sync_calls.load(std::memory_order_relaxed);
  result.sync_errors =
      impl_->health_sync_errors.load(std::memory_order_relaxed);
  result.checkpoints =
      impl_->health_checkpoints.load(std::memory_order_relaxed);
  return result;
}

Status RingStore::writer_status() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->writable)
    return Status::Error(StatusCode::invalid_argument, "store is read-only");
  if (impl_->closed)
    return Status::Error(StatusCode::closed, "store is closed");
  if (impl_->faulted) return impl_->fault_status;
  return Status::Ok();
}

}  // namespace tfdb

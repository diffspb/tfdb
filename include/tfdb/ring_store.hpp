#ifndef TFDB_RING_STORE_HPP
#define TFDB_RING_STORE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "tfdb/bytes.hpp"
#include "tfdb/codec.hpp"
#include "tfdb/status.hpp"
#include "tfdb/storage.hpp"

namespace tfdb {

constexpr std::uint32_t kBlockFlagTimeAnomaly = 1u << 0;
constexpr std::uint32_t kBlockFlagUnsynchronizedTime = 1u << 1;
constexpr std::uint32_t kBlockFlagApplicationBase = 1u << 16;

constexpr std::uint32_t kRecordFlagUnsynchronizedTime = 1u << 0;

struct VolumeOptions {
  std::uint64_t partition_size = 64ull * 1024ull * 1024ull;
  std::uint32_t index_region_size = 1024u * 1024u;
  std::uint32_t max_block_payload = 32u * 1024u;
  // Logical separation between block starts. This is not an O_DIRECT buffer
  // alignment requirement. It prevents a later block from dirtying the same
  // persistence unit as an already checkpointed block.
  std::uint32_t persistence_quantum = 4096u;
  std::uint64_t volume_id_high = 0;
  std::uint64_t volume_id_low = 0;
  // Deterministic IDs are only for golden vectors/tests. Production format
  // creates a fresh 128-bit incarnation ID with getrandom().
  bool allow_explicit_volume_id_for_testing = false;
  std::int64_t created_time_ns = 0;
};

struct PartitionOptions {
  CompressionId compression = CompressionId::none;
  std::uint16_t compression_version = 1;
  std::uint64_t record_format_id = 0;
  std::uint16_t record_format_version = 0;
  // Timestamps are signed nanoseconds. 1 is Unix realtime; other values are
  // project/profile-defined and must remain stable for the partition.
  std::uint64_t time_domain_id = 1;
  // Negative disables the corresponding diagnostic threshold.
  std::int64_t allowed_backward_skew_ns = -1;
  std::int64_t allowed_forward_step_ns = -1;
};

struct AppendContract {
  std::uint64_t record_format_id = 0;
  std::uint16_t record_format_version = 0;
  std::uint64_t time_domain_id = 1;
};

struct OpenOptions {
  bool writable = false;
  bool verify_payloads_on_open = false;
  PartitionOptions next_partition;
  std::vector<std::shared_ptr<const CompressionCodec>> compression_codecs;
  // Deterministic writer IDs are restricted to recovery/golden tests.
  std::uint64_t writer_id_high = 0;
  std::uint64_t writer_id_low = 0;
  bool allow_explicit_writer_id_for_testing = false;
};

struct StoreMetrics {
  std::uint64_t accepted_records = 0;
  std::uint64_t accepted_payload_bytes = 0;
  std::uint64_t published_blocks = 0;
  std::uint64_t stored_block_bytes = 0;
  std::uint64_t raw_block_bytes = 0;
  std::uint64_t checkpoints = 0;
  std::uint64_t rotations = 0;
  std::uint64_t time_anomalies = 0;
  std::uint64_t overwritten_gaps = 0;
  std::uint64_t corrupt_gaps = 0;
  std::uint64_t durable_records = 0;
  std::uint64_t durable_blocks = 0;
  std::uint64_t sync_errors = 0;
  std::uint64_t sync_calls = 0;
  std::uint64_t last_sync_duration_ns = 0;
  std::uint64_t max_sync_duration_ns = 0;
  std::uint64_t oldest_undurable_age_ns = 0;
  std::uint64_t max_accepted_to_durable_ns = 0;
};

struct BlockMetadata {
  std::uint32_t partition_slot = 0;
  std::uint64_t partition_generation = 0;
  std::uint32_t block_sequence = 0;
  std::uint32_t record_count = 0;
  std::int64_t min_time_ns = 0;
  std::int64_t max_time_ns = 0;
  std::uint32_t flags = 0;
  CompressionId compression = CompressionId::none;
  std::uint32_t stored_size = 0;
  std::uint32_t raw_size = 0;
  std::uint64_t physical_offset = 0;
  std::uint64_t record_format_id = 0;
  std::uint16_t record_format_version = 0;
  std::uint64_t time_domain_id = 0;
};

enum class BlockEventKind {
  data,
  overwritten_gap,
  corrupt_gap
};

struct BlockEvent {
  BlockEventKind kind = BlockEventKind::data;
  BlockMetadata metadata;
  // Valid only for the duration of the BlockVisitor invocation. Copy bytes
  // that must outlive the callback.
  ByteView data;
  Status detail;
};

struct TimeRange {
  std::int64_t begin_ns = 0;
  std::int64_t end_ns = 0;  // Half-open: [begin_ns, end_ns).
  std::uint64_t time_domain_id = 1;
};

struct QueryOptions {
  bool continue_on_gap = true;
  // Deprecated and ignored. CRC verification is unconditional on the live
  // ring; setting this to false has never disabled anything. Kept as a plain
  // field rather than [[deprecated]] because a deprecated member with a
  // default initializer warns on every QueryOptions construction, including
  // correct ones. Scheduled for removal with the next incompatible API change.
  bool verify_payload_crc = true;
};

using BlockVisitor = std::function<bool(const BlockEvent&)>;

struct PartitionInfo {
  std::uint32_t slot = 0;
  std::uint64_t generation = 0;
  bool sealed = false;
  std::uint32_t block_count = 0;
  std::uint64_t data_bytes = 0;
  std::int64_t min_time_ns = 0;
  std::int64_t max_time_ns = 0;
  std::uint32_t flags = 0;
  PartitionOptions options;
};

struct VolumeInfo {
  std::uint64_t volume_size = 0;
  std::uint64_t partition_size = 0;
  std::uint32_t partition_count = 0;
  std::uint32_t index_region_size = 0;
  std::uint32_t max_block_payload = 0;
  std::uint32_t persistence_quantum = 0;
  std::uint64_t volume_id_high = 0;
  std::uint64_t volume_id_low = 0;
  std::vector<PartitionInfo> partitions;
};

class RingStore {
 public:
  ~RingStore();

  static Status format(Storage& storage, const VolumeOptions& options);
  static Status open(std::shared_ptr<Storage> storage,
                     const OpenOptions& options,
                     std::unique_ptr<RingStore>* output);

  Status append(ByteView encoded_record, std::int64_t index_time_ns,
                std::uint32_t record_flags = 0);
  // Like append(), but atomically verifies the partition contract after any
  // automatic rotation and before copying the record into the builder.
  Status append_checked(ByteView encoded_record, std::int64_t index_time_ns,
                        const AppendContract& expected,
                        std::uint32_t record_flags = 0);
  Status publish();
  Status checkpoint();
  // Deprecated: an exact alias for checkpoint(), kept only for source
  // compatibility. The name suggested a distinct emergency path that has never
  // existed; hold-up-time budgeting is a deployment property measured against
  // checkpoint() itself.
  [[deprecated("exact alias for checkpoint(); call checkpoint()")]]
  Status emergency_checkpoint() { return checkpoint(); }
  // Graceful writer shutdown. Unlike the destructor, this checkpoints a RAM
  // tail. Subsequent mutating calls return StatusCode::closed.
  Status close();
  Status rotate(const PartitionOptions& next_options);
  Status set_next_partition_options(const PartitionOptions& options);

  Status query_blocks(const TimeRange& range, const QueryOptions& options,
                      const BlockVisitor& visitor) const;
  Status scan_blocks(const QueryOptions& options,
                     const BlockVisitor& visitor) const;

  Status inspect(VolumeInfo* output) const;
  Status active_partition_info(PartitionInfo* output) const;
  StoreMetrics metrics() const;
  // Returns OK only while this writable store can accept mutations. Unlike
  // writable(), this reports closed and the original fault status.
  Status writer_status() const;
  bool writable() const { return writable_; }

 private:
  struct Impl;
  explicit RingStore(std::unique_ptr<Impl> impl);
  Status append_impl(ByteView encoded_record, std::int64_t index_time_ns,
                     std::uint32_t record_flags,
                     const AppendContract* expected);

  std::unique_ptr<Impl> impl_;
  bool writable_;
};

}  // namespace tfdb

#endif  // TFDB_RING_STORE_HPP

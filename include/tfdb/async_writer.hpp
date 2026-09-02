#ifndef TFDB_ASYNC_WRITER_HPP
#define TFDB_ASYNC_WRITER_HPP

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "tfdb/ring_store.hpp"

namespace tfdb {

struct AsyncWriterOptions {
  std::size_t queue_capacity_records = 4096;
  std::size_t queue_capacity_bytes = 4u * 1024u * 1024u;
  std::chrono::milliseconds checkpoint_interval{1000};
  // Optional monotonic nanosecond clock used by both the checkpoint deadline
  // and latency metrics. Empty selects std::chrono::steady_clock. A custom
  // clock must be nonblocking, non-throwing, monotonic, and non-reentrant. It
  // must be advanced externally followed by notify_clock_advanced(); release
  // any mutex used by the clock before notifying the writer.
  std::function<std::uint64_t()> monotonic_clock_ns;
};

struct AsyncWriterMetrics {
  std::uint64_t submitted_records = 0;
  std::uint64_t processed_records = 0;
  std::uint64_t durable_sequence = 0;
  std::uint64_t checkpoint_calls = 0;
  std::uint64_t background_errors = 0;
  std::uint64_t backpressure_events = 0;
  std::uint64_t maximum_queued_records = 0;
  std::uint64_t maximum_queued_bytes = 0;
  // Time from acceptance until dequeue, excluding RingStore/backend work.
  std::uint64_t maximum_queue_residence_ns = 0;
  std::uint64_t maximum_accepted_to_durable_ns = 0;
  std::uint64_t oldest_undurable_age_ns = 0;
};

class AsyncWriter {
 public:
  AsyncWriter(RingStore& store, const AsyncWriterOptions& options);
  ~AsyncWriter();

  Status start();
  Status submit(ByteView encoded_record, std::int64_t index_time_ns,
                std::uint32_t record_flags = 0);
  // Queues a record together with the partition contract that must still hold
  // after any automatic rotation performed by the background thread.
  Status submit_checked(ByteView encoded_record, std::int64_t index_time_ns,
                        const AppendContract& expected,
                        std::uint32_t record_flags = 0);
  Status checkpoint();
  Status stop();
  // Wakes a writer using a custom monotonic clock so it can re-evaluate its
  // deadline. Harmless with the default clock.
  void notify_clock_advanced();

  std::size_t queued_records() const;
  std::size_t queued_bytes() const;
  Status background_status() const;
  AsyncWriterMetrics metrics() const;

 private:
  struct Item {
    std::vector<std::uint8_t> bytes;
    std::int64_t time_ns = 0;
    std::uint32_t flags = 0;
    std::uint64_t sequence = 0;
    bool checked = false;
    AppendContract contract;
    std::uint64_t submitted_at_ns = 0;
  };

  void run();
  Status submit_impl(ByteView encoded_record, std::int64_t index_time_ns,
                     std::uint32_t record_flags,
                     const AppendContract* expected);

  RingStore& store_;
  AsyncWriterOptions options_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::condition_variable barrier_condition_;
  std::deque<Item> queue_;
  std::size_t queued_bytes_;
  std::thread thread_;
  bool running_;
  bool stopping_;
  bool checkpoint_requested_;
  std::uint64_t submitted_sequence_;
  std::uint64_t processed_sequence_;
  std::uint64_t requested_barrier_;
  std::uint64_t completed_barrier_;
  bool have_oldest_undurable_;
  std::uint64_t oldest_undurable_ns_;
  bool custom_clock_;
  AsyncWriterMetrics metrics_;
  Status background_status_;
};

}  // namespace tfdb

#endif  // TFDB_ASYNC_WRITER_HPP

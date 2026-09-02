#include "tfdb/async_writer.hpp"

#include <algorithm>

namespace tfdb {
namespace {

std::uint64_t steady_now_ns() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t elapsed_ns(std::uint64_t begin, std::uint64_t end) {
  return end >= begin ? end - begin : 0;
}

bool interval_elapsed(std::uint64_t started, std::uint64_t now,
                      std::uint64_t interval) {
  // The injected clock is required to be monotonic. Keeping the start instant
  // avoids an absolute-deadline saturation loop when now reaches UINT64_MAX.
  return now >= started && now - started >= interval;
}

}  // namespace

AsyncWriter::AsyncWriter(RingStore& store, const AsyncWriterOptions& options)
    : store_(store), options_(options), queued_bytes_(0), running_(false),
      stopping_(false), checkpoint_requested_(false), submitted_sequence_(0),
      processed_sequence_(0), requested_barrier_(0), completed_barrier_(0),
      have_oldest_undurable_(false), oldest_undurable_ns_(0),
      custom_clock_(static_cast<bool>(options.monotonic_clock_ns)) {
  if (!options_.monotonic_clock_ns)
    options_.monotonic_clock_ns = steady_now_ns;
}

AsyncWriter::~AsyncWriter() { (void)stop(); }

Status AsyncWriter::start() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (running_) return Status::Error(StatusCode::busy, "async writer already running");
  if (thread_.joinable()) {
    lock.unlock();
    thread_.join();
    lock.lock();
  }
  // A background failure also faults RingStore. Restarting the wrapper would
  // otherwise appear successful until the next dequeued item failed. Preserve
  // the original diagnostic and require reopening the store/new wrapper.
  if (!background_status_.ok()) return background_status_;
  Status store_status = store_.writer_status();
  if (!store_status.ok()) return store_status;
  constexpr std::uint64_t kNanosecondsPerMillisecond = 1000000;
  const auto interval_ms = options_.checkpoint_interval.count();
  if (options_.queue_capacity_records == 0 || options_.queue_capacity_bytes == 0 ||
      interval_ms <= 0 ||
      static_cast<std::uint64_t>(interval_ms) >
          UINT64_MAX / kNanosecondsPerMillisecond)
    return Status::Error(StatusCode::invalid_argument, "invalid async writer options");
  stopping_ = false;
  checkpoint_requested_ = false;
  queue_.clear();
  queued_bytes_ = 0;
  submitted_sequence_ = 0;
  processed_sequence_ = 0;
  requested_barrier_ = 0;
  completed_barrier_ = 0;
  have_oldest_undurable_ = false;
  metrics_ = AsyncWriterMetrics();
  background_status_ = Status::Ok();
  running_ = true;
  thread_ = std::thread(&AsyncWriter::run, this);
  return Status::Ok();
}

Status AsyncWriter::submit(ByteView record, std::int64_t time,
                           std::uint32_t flags) {
  return submit_impl(record, time, flags, nullptr);
}

Status AsyncWriter::submit_checked(ByteView record, std::int64_t time,
                                   const AppendContract& expected,
                                   std::uint32_t flags) {
  return submit_impl(record, time, flags, &expected);
}

Status AsyncWriter::submit_impl(ByteView record, std::int64_t time,
                                std::uint32_t flags,
                                const AppendContract* expected) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!background_status_.ok()) return background_status_;
  if (!running_ || stopping_) return Status::Error(StatusCode::closed, "async writer is not accepting");
  if (record.empty()) return Status::Error(StatusCode::invalid_argument, "zero-length record");
  if (!record.valid()) return Status::Error(StatusCode::invalid_argument,
                                            "null non-empty record");
  if (queue_.size() >= options_.queue_capacity_records ||
      record.size() > options_.queue_capacity_bytes - std::min(queued_bytes_, options_.queue_capacity_bytes)) {
    ++metrics_.backpressure_events;
    return Status::Error(StatusCode::busy, "async writer queue is full");
  }
  Item item;
  item.bytes.assign(record.data(), record.data() + record.size());
  item.time_ns = time;
  item.flags = flags;
  item.sequence = ++submitted_sequence_;
  item.checked = expected != nullptr;
  if (expected) item.contract = *expected;
  item.submitted_at_ns = options_.monotonic_clock_ns();
  queued_bytes_ += item.bytes.size();
  queue_.push_back(std::move(item));
  if (!have_oldest_undurable_) {
    have_oldest_undurable_ = true;
    oldest_undurable_ns_ = queue_.back().submitted_at_ns;
  }
  ++metrics_.submitted_records;
  metrics_.maximum_queued_records = std::max<std::uint64_t>(
      metrics_.maximum_queued_records, queue_.size());
  metrics_.maximum_queued_bytes = std::max<std::uint64_t>(
      metrics_.maximum_queued_bytes, queued_bytes_);
  condition_.notify_one();
  return Status::Ok();
}

Status AsyncWriter::checkpoint() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!background_status_.ok()) return background_status_;
  if (!running_) return Status::Error(StatusCode::closed, "async writer is not running");
  const std::uint64_t target = submitted_sequence_;
  requested_barrier_ = std::max(requested_barrier_, target);
  checkpoint_requested_ = true;
  condition_.notify_one();
  barrier_condition_.wait(lock, [&] {
    return completed_barrier_ >= target || !background_status_.ok() || !running_;
  });
  if (!background_status_.ok()) return background_status_;
  if (completed_barrier_ < target)
    return Status::Error(StatusCode::closed, "async writer stopped before checkpoint");
  return Status::Ok();
}

Status AsyncWriter::stop() {
  bool was_running = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    was_running = running_;
    if (running_) {
      stopping_ = true;
      requested_barrier_ = submitted_sequence_;
      checkpoint_requested_ = true;
      condition_.notify_one();
    }
  }
  if (thread_.joinable()) thread_.join();
  std::lock_guard<std::mutex> lock(mutex_);
  (void)was_running;
  return background_status_;
}

void AsyncWriter::notify_clock_advanced() {
  std::lock_guard<std::mutex> lock(mutex_);
  condition_.notify_all();
}

std::size_t AsyncWriter::queued_records() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

std::size_t AsyncWriter::queued_bytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queued_bytes_;
}

Status AsyncWriter::background_status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return background_status_;
}

AsyncWriterMetrics AsyncWriter::metrics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  AsyncWriterMetrics result = metrics_;
  if (have_oldest_undurable_) {
    result.oldest_undurable_age_ns = elapsed_ns(
        oldest_undurable_ns_, options_.monotonic_clock_ns());
  }
  return result;
}

void AsyncWriter::run() {
  constexpr std::uint64_t kNanosecondsPerMillisecond = 1000000;
  const std::uint64_t interval_ns =
      static_cast<std::uint64_t>(options_.checkpoint_interval.count()) *
      kNanosecondsPerMillisecond;
  std::uint64_t checkpoint_started_ns = options_.monotonic_clock_ns();
  for (;;) {
    Item item;
    bool have_item = false;
    bool do_checkpoint = false;
    bool exit_after_checkpoint = false;
    std::uint64_t dequeued_at_ns = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (custom_clock_) {
        condition_.wait(lock, [&] {
          return stopping_ || checkpoint_requested_ || !queue_.empty() ||
                 interval_elapsed(checkpoint_started_ns,
                                  options_.monotonic_clock_ns(), interval_ns);
        });
      } else {
        const std::uint64_t now_ns = options_.monotonic_clock_ns();
        const std::uint64_t elapsed = now_ns >= checkpoint_started_ns
            ? now_ns - checkpoint_started_ns : 0;
        const std::uint64_t remaining_ns = elapsed >= interval_ns
            ? 0 : interval_ns - elapsed;
        // libstdc++ implements wait_for as steady_clock::now() + duration;
        // passing INT64_MAX nanoseconds can overflow that internal addition.
        // Long configured intervals are therefore waited in harmless chunks.
        constexpr std::uint64_t kMaximumWaitChunkNs =
            60ull * 60ull * 1000000000ull;
        const std::uint64_t bounded_ns = std::min(
            remaining_ns, kMaximumWaitChunkNs);
        condition_.wait_for(lock,
                            std::chrono::nanoseconds(
                                static_cast<std::int64_t>(bounded_ns)), [&] {
          return stopping_ || checkpoint_requested_ || !queue_.empty();
        });
      }
      const std::uint64_t now_ns = options_.monotonic_clock_ns();
      if (interval_elapsed(checkpoint_started_ns, now_ns, interval_ns))
        do_checkpoint = true;
      if (checkpoint_requested_ && processed_sequence_ >= requested_barrier_)
        do_checkpoint = true;
      if (stopping_ && queue_.empty()) {
        do_checkpoint = true;
        exit_after_checkpoint = true;
      }
      if (!do_checkpoint && !queue_.empty()) {
        item = std::move(queue_.front());
        queue_.pop_front();
        queued_bytes_ -= item.bytes.size();
        have_item = true;
        dequeued_at_ns = options_.monotonic_clock_ns();
      }
    }

    Status status;
    if (have_item) {
      status = item.checked
          ? store_.append_checked(ByteView(item.bytes), item.time_ns,
                                  item.contract, item.flags)
          : store_.append(ByteView(item.bytes), item.time_ns, item.flags);
    }
    if (do_checkpoint) status = store_.checkpoint();

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!status.ok()) {
        ++metrics_.background_errors;
        background_status_ = status;
        running_ = false;
        stopping_ = true;
        queue_.clear();
        queued_bytes_ = 0;
        barrier_condition_.notify_all();
        return;
      }
      if (have_item) {
        processed_sequence_ = item.sequence;
        ++metrics_.processed_records;
        const std::uint64_t residence = elapsed_ns(
            item.submitted_at_ns, dequeued_at_ns);
        metrics_.maximum_queue_residence_ns = std::max(
            metrics_.maximum_queue_residence_ns, residence);
      }
      if (do_checkpoint) {
        ++metrics_.checkpoint_calls;
        const bool advanced = processed_sequence_ > metrics_.durable_sequence;
        completed_barrier_ = processed_sequence_;
        const std::uint64_t durable_at_ns = options_.monotonic_clock_ns();
        if (advanced && have_oldest_undurable_) {
          const std::uint64_t age = elapsed_ns(oldest_undurable_ns_,
                                               durable_at_ns);
          metrics_.maximum_accepted_to_durable_ns = std::max(
              metrics_.maximum_accepted_to_durable_ns, age);
          if (queue_.empty()) {
            have_oldest_undurable_ = false;
          } else {
            oldest_undurable_ns_ = queue_.front().submitted_at_ns;
          }
        }
        metrics_.durable_sequence = completed_barrier_;
        if (completed_barrier_ >= requested_barrier_) checkpoint_requested_ = false;
        checkpoint_started_ns = durable_at_ns;
        barrier_condition_.notify_all();
      }
      if (exit_after_checkpoint) {
        running_ = false;
        barrier_condition_.notify_all();
        return;
      }
    }
  }
}

}  // namespace tfdb

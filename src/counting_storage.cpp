#include "tfdb/counting_storage.hpp"

namespace tfdb {

IoResult CountingStorage::read_at(std::uint64_t offset, MutableByteView output) {
  IoResult result = delegate_->read_at(offset, output);
  std::lock_guard<std::mutex> lock(mutex_);
  ++counters_.read_calls;
  counters_.read_bytes += result.transferred;
  return result;
}

IoResult CountingStorage::write_at(std::uint64_t offset, ByteView input) {
  IoResult result = delegate_->write_at(offset, input);
  std::lock_guard<std::mutex> lock(mutex_);
  ++counters_.write_calls;
  counters_.write_bytes += result.transferred;
  return result;
}

Status CountingStorage::flush() {
  Status status = delegate_->flush();
  std::lock_guard<std::mutex> lock(mutex_);
  ++counters_.flush_calls;
  return status;
}

StorageCounters CountingStorage::counters() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return counters_;
}

void CountingStorage::reset_counters() {
  std::lock_guard<std::mutex> lock(mutex_);
  counters_ = StorageCounters();
}

}  // namespace tfdb

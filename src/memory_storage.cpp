#include "tfdb/memory_storage.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace tfdb {
namespace {

std::size_t checked_memory_size(std::uint64_t size) {
  if (size > std::numeric_limits<std::size_t>::max())
    throw std::length_error("MemoryStorage size exceeds address space");
  return static_cast<std::size_t>(size);
}

}  // namespace

MemoryStorage::MemoryStorage(std::uint64_t size, bool writable)
    : volatile_(checked_memory_size(size), 0),
      durable_(checked_memory_size(size), 0),
      writable_(writable) {}

std::uint64_t MemoryStorage::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return volatile_.size();
}

const FaultRule* MemoryStorage::matching_fault(StorageOperation operation,
                                                std::uint64_t call) const {
  for (const FaultRule& rule : faults_) {
    if (rule.operation == operation && rule.call == call) return &rule;
  }
  return nullptr;
}

IoResult MemoryStorage::read_at(std::uint64_t offset, MutableByteView output) {
  if (!output.valid())
    return {0, Status::Error(StatusCode::invalid_argument,
                             "null non-empty memory read buffer")};
  std::uint64_t call = 0;
  StorageHook hook;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    call = ++counters_.read_calls;
    hook = hook_;
  }
  if (hook) hook(StorageOperation::read, HookPoint::before, call, offset, output.size());
  IoResult result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (offset > volatile_.size() || output.size() > volatile_.size() - offset) {
      result = {0, Status::Error(StatusCode::out_of_range, "memory read outside image")};
    } else {
      const FaultRule* fault = matching_fault(StorageOperation::read, call);
      std::size_t transfer = output.size();
      Status status;
      if (fault) {
        if (fault->result == StatusCode::ok) {
          transfer = std::min(fault->transfer_before_error, output.size());
        } else {
          transfer = 0;  // POSIX-style error has no transferred byte count.
          status = Status::Error(fault->result, "injected read fault");
        }
      }
      if (transfer != 0) {
        std::copy(volatile_.begin() + static_cast<std::size_t>(offset),
                  volatile_.begin() + static_cast<std::size_t>(offset) + transfer,
                  output.data());
      }
      counters_.read_bytes += transfer;
      result = {transfer, status};
    }
    events_.push_back({StorageOperation::read, call, offset, output.size(),
                       result.transferred, result.status.code()});
  }
  if (hook) hook(StorageOperation::read, HookPoint::after, call, offset, output.size());
  return result;
}

IoResult MemoryStorage::write_at(std::uint64_t offset, ByteView input) {
  if (!input.valid())
    return {0, Status::Error(StatusCode::invalid_argument,
                             "null non-empty memory write buffer")};
  std::uint64_t call = 0;
  StorageHook hook;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    call = ++counters_.write_calls;
    hook = hook_;
  }
  if (hook) hook(StorageOperation::write, HookPoint::before, call, offset, input.size());
  IoResult result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!writable_) {
      result = {0, Status::Error(StatusCode::io_error, "memory storage is read-only")};
    } else if (offset > volatile_.size() || input.size() > volatile_.size() - offset) {
      result = {0, Status::Error(StatusCode::no_space, "memory write outside image")};
    } else {
      const FaultRule* fault = matching_fault(StorageOperation::write, call);
      std::size_t transfer = input.size();
      Status status;
      if (fault) {
        if (fault->result == StatusCode::ok) {
          transfer = std::min(fault->transfer_before_error, input.size());
        } else {
          transfer = 0;
          status = Status::Error(fault->result, "injected write fault");
        }
      }
      if (transfer != 0) {
        std::copy(input.data(), input.data() + transfer,
                  volatile_.begin() + static_cast<std::size_t>(offset));
        DirtyWrite dirty;
        dirty.call = call;
        dirty.offset = offset;
        dirty.bytes.assign(input.data(), input.data() + transfer);
        dirty_writes_.push_back(std::move(dirty));
      }
      counters_.write_bytes += transfer;
      result = {transfer, status};
    }
    events_.push_back({StorageOperation::write, call, offset, input.size(),
                       result.transferred, result.status.code()});
  }
  if (hook) hook(StorageOperation::write, HookPoint::after, call, offset, input.size());
  return result;
}

Status MemoryStorage::flush() {
  std::uint64_t call = 0;
  StorageHook hook;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    call = ++counters_.flush_calls;
    hook = hook_;
  }
  if (hook) hook(StorageOperation::flush, HookPoint::before, call, 0, 0);
  Status status;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const FaultRule* fault = matching_fault(StorageOperation::flush, call);
    if (fault && fault->result != StatusCode::ok) {
      status = Status::Error(fault->result, "injected flush fault");
    } else {
      durable_ = volatile_;
      dirty_writes_.clear();
    }
    events_.push_back({StorageOperation::flush, call, 0, 0, 0, status.code()});
  }
  if (hook) hook(StorageOperation::flush, HookPoint::after, call, 0, 0);
  return status;
}

void MemoryStorage::add_fault(const FaultRule& rule) {
  std::lock_guard<std::mutex> lock(mutex_);
  faults_.push_back(rule);
}

void MemoryStorage::clear_faults() {
  std::lock_guard<std::mutex> lock(mutex_);
  faults_.clear();
}

void MemoryStorage::set_hook(const StorageHook& hook) {
  std::lock_guard<std::mutex> lock(mutex_);
  hook_ = hook;
}

void MemoryStorage::crash_discard_volatile() {
  std::lock_guard<std::mutex> lock(mutex_);
  volatile_ = durable_;
  dirty_writes_.clear();
}

void MemoryStorage::crash_persist_all_dirty() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const DirtyWrite& write : dirty_writes_)
    std::copy(write.bytes.begin(), write.bytes.end(),
              durable_.begin() + static_cast<std::size_t>(write.offset));
  volatile_ = durable_;
  dirty_writes_.clear();
}

Status MemoryStorage::crash_materialize(const std::vector<WriteFragment>& fragments) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<const DirtyWrite*> selected_writes;
  selected_writes.reserve(fragments.size());
  for (const WriteFragment& fragment : fragments) {
    const DirtyWrite* selected = nullptr;
    for (const DirtyWrite& write : dirty_writes_)
      if (write.call == fragment.write_call) { selected = &write; break; }
    if (!selected || fragment.offset_in_write > selected->bytes.size() ||
        fragment.length > selected->bytes.size() - fragment.offset_in_write) {
      return Status::Error(StatusCode::invalid_argument, "invalid dirty write fragment");
    }
    const std::uint64_t destination = selected->offset + fragment.offset_in_write;
    if (destination > durable_.size() || fragment.length > durable_.size() - destination)
      return Status::Error(StatusCode::out_of_range, "dirty fragment outside image");
    selected_writes.push_back(selected);
  }
  std::vector<std::uint8_t> materialized = durable_;
  for (std::size_t i = 0; i != fragments.size(); ++i) {
    const WriteFragment& fragment = fragments[i];
    const DirtyWrite& selected = *selected_writes[i];
    const std::uint64_t destination = selected.offset + fragment.offset_in_write;
    std::copy(selected.bytes.begin() + fragment.offset_in_write,
              selected.bytes.begin() + fragment.offset_in_write + fragment.length,
              materialized.begin() + static_cast<std::size_t>(destination));
  }
  durable_.swap(materialized);
  volatile_ = durable_;
  dirty_writes_.clear();
  return Status::Ok();
}

Status MemoryStorage::corrupt_durable(std::uint64_t offset,
                                      std::uint8_t xor_mask) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (offset >= durable_.size())
    return Status::Error(StatusCode::out_of_range, "durable corruption outside image");
  durable_[static_cast<std::size_t>(offset)] ^= xor_mask;
  return Status::Ok();
}

Status MemoryStorage::corrupt_volatile(std::uint64_t offset,
                                       std::uint8_t xor_mask) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (offset >= volatile_.size())
    return Status::Error(StatusCode::out_of_range, "volatile corruption outside image");
  volatile_[static_cast<std::size_t>(offset)] ^= xor_mask;
  return Status::Ok();
}

StorageCounters MemoryStorage::counters() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return counters_;
}

void MemoryStorage::reset_counters() {
  std::lock_guard<std::mutex> lock(mutex_);
  counters_ = StorageCounters();
  events_.clear();
  // WriteFragment IDs are relative to the current measurement epoch. Dirty
  // writes from an earlier epoch must not alias newly numbered writes.
  dirty_writes_.clear();
}

std::vector<StorageEvent> MemoryStorage::events() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return events_;
}

std::vector<std::uint8_t> MemoryStorage::durable_image() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return durable_;
}

std::vector<std::uint8_t> MemoryStorage::volatile_image() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return volatile_;
}

}  // namespace tfdb

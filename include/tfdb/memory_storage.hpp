// Copyright (c) 2026 Alexander Safronenko
// SPDX-License-Identifier: BSD-2-Clause

#ifndef TFDB_MEMORY_STORAGE_HPP
#define TFDB_MEMORY_STORAGE_HPP

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <functional>
#include <vector>

#include "tfdb/storage.hpp"

namespace tfdb {

enum class StorageOperation { read, write, flush };

struct StorageCounters {
  std::uint64_t read_calls = 0;
  std::uint64_t write_calls = 0;
  std::uint64_t flush_calls = 0;
  std::uint64_t read_bytes = 0;
  std::uint64_t write_bytes = 0;
};

struct FaultRule {
  StorageOperation operation = StorageOperation::write;
  std::uint64_t call = 0;  // One-based call number for this operation.
  std::size_t transfer_before_error = 0;
  StatusCode result = StatusCode::io_error;
};

struct StorageEvent {
  StorageOperation operation = StorageOperation::read;
  std::uint64_t call = 0;
  std::uint64_t offset = 0;
  std::size_t requested = 0;
  std::size_t transferred = 0;
  StatusCode result = StatusCode::ok;
};

struct WriteFragment {
  std::uint64_t write_call = 0;
  std::size_t offset_in_write = 0;
  std::size_t length = 0;
};

enum class HookPoint { before, after };
using StorageHook = std::function<void(StorageOperation, HookPoint,
                                       std::uint64_t, std::uint64_t,
                                       std::size_t)>;

class MemoryStorage final : public Storage {
 public:
  explicit MemoryStorage(std::uint64_t size, bool writable = true);

  std::uint64_t size() const override;
  bool writable() const override { return writable_; }
  IoResult read_at(std::uint64_t offset, MutableByteView output) override;
  IoResult write_at(std::uint64_t offset, ByteView input) override;
  Status flush() override;

  void add_fault(const FaultRule& rule);
  void clear_faults();
  void set_hook(const StorageHook& hook);
  void crash_discard_volatile();
  void crash_persist_all_dirty();
  // Applies selected pieces of successful writes since the last flush in the
  // provided order, then discards everything else. This can model torn,
  // reordered, or sector-subset persistence.
  Status crash_materialize(const std::vector<WriteFragment>& fragments);
  Status corrupt_durable(std::uint64_t offset, std::uint8_t xor_mask);
  Status corrupt_volatile(std::uint64_t offset, std::uint8_t xor_mask);
  StorageCounters counters() const;
  // Starts a new measurement/fault-call-number epoch. Existing volatile bytes
  // and fault rules remain; pre-epoch writes are no longer candidates for
  // crash_materialize(). Call clear_faults() explicitly when rules must not
  // fire again after their one-based call numbers restart.
  void reset_counters();
  std::vector<StorageEvent> events() const;
  std::vector<std::uint8_t> durable_image() const;
  std::vector<std::uint8_t> volatile_image() const;

 private:
  const FaultRule* matching_fault(StorageOperation operation,
                                  std::uint64_t call) const;

  struct DirtyWrite {
    std::uint64_t call = 0;
    std::uint64_t offset = 0;
    std::vector<std::uint8_t> bytes;
  };

  mutable std::mutex mutex_;
  std::vector<std::uint8_t> volatile_;
  std::vector<std::uint8_t> durable_;
  bool writable_;
  std::vector<FaultRule> faults_;
  std::vector<DirtyWrite> dirty_writes_;
  std::vector<StorageEvent> events_;
  StorageHook hook_;
  StorageCounters counters_;
};

}  // namespace tfdb

#endif  // TFDB_MEMORY_STORAGE_HPP

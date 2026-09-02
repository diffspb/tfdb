#ifndef TFDB_COUNTING_STORAGE_HPP
#define TFDB_COUNTING_STORAGE_HPP

#include <memory>
#include <mutex>

#include "tfdb/memory_storage.hpp"
#include "tfdb/storage.hpp"

namespace tfdb {

class CountingStorage final : public Storage {
 public:
  explicit CountingStorage(std::shared_ptr<Storage> delegate)
      : delegate_(std::move(delegate)) {}

  std::uint64_t size() const override { return delegate_->size(); }
  bool writable() const override { return delegate_->writable(); }
  IoResult read_at(std::uint64_t offset, MutableByteView output) override;
  IoResult write_at(std::uint64_t offset, ByteView input) override;
  Status flush() override;

  StorageCounters counters() const;
  void reset_counters();

 private:
  std::shared_ptr<Storage> delegate_;
  mutable std::mutex mutex_;
  StorageCounters counters_;
};

}  // namespace tfdb

#endif  // TFDB_COUNTING_STORAGE_HPP

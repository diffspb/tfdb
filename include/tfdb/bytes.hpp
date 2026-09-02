#ifndef TFDB_BYTES_HPP
#define TFDB_BYTES_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tfdb {

class ByteView {
 public:
  ByteView() : data_(nullptr), size_(0) {}
  ByteView(const void* data, std::size_t size)
      : data_(static_cast<const std::uint8_t*>(data)), size_(size) {}
  explicit ByteView(const std::vector<std::uint8_t>& value)
      : data_(value.empty() ? nullptr : value.data()), size_(value.size()) {}

  const std::uint8_t* data() const { return data_; }
  std::size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  bool valid() const { return size_ == 0 || data_ != nullptr; }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
};

class MutableByteView {
 public:
  MutableByteView() : data_(nullptr), size_(0) {}
  MutableByteView(void* data, std::size_t size)
      : data_(static_cast<std::uint8_t*>(data)), size_(size) {}

  std::uint8_t* data() const { return data_; }
  std::size_t size() const { return size_; }
  bool valid() const { return size_ == 0 || data_ != nullptr; }

 private:
  std::uint8_t* data_;
  std::size_t size_;
};

}  // namespace tfdb

#endif  // TFDB_BYTES_HPP

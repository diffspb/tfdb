// Copyright (c) 2026 Alexander Safronenko
// SPDX-License-Identifier: BSD-2-Clause

#ifndef TFDB_STORAGE_HPP
#define TFDB_STORAGE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "tfdb/bytes.hpp"
#include "tfdb/status.hpp"

namespace tfdb {

struct IoResult {
  std::size_t transferred = 0;
  Status status;
};

class Storage {
 public:
  virtual ~Storage() {}

  // Backends used by RingStore must allow concurrent positional reads and a
  // single positional writer. A successful flush makes every preceding
  // successful write durable according to the backend/device contract.
  virtual std::uint64_t size() const = 0;
  virtual bool writable() const = 0;
  virtual IoResult read_at(std::uint64_t offset, MutableByteView output) = 0;
  virtual IoResult write_at(std::uint64_t offset, ByteView input) = 0;
  virtual Status flush() = 0;
};

class FileStorage final : public Storage {
 public:
  ~FileStorage() override;

  static Status open_existing(const std::string& path, bool writable,
                              std::shared_ptr<FileStorage>* output);
  static Status create(const std::string& path, std::uint64_t size,
                       bool overwrite, std::shared_ptr<FileStorage>* output);

  std::uint64_t size() const override { return size_; }
  bool writable() const override { return writable_; }
  IoResult read_at(std::uint64_t offset, MutableByteView output) override;
  IoResult write_at(std::uint64_t offset, ByteView input) override;
  Status flush() override;

 private:
  FileStorage(int fd, std::uint64_t size, bool writable)
      : fd_(fd), size_(size), writable_(writable) {}

  int fd_;
  std::uint64_t size_;
  bool writable_;
};

Status read_exact(Storage& storage, std::uint64_t offset,
                  MutableByteView output);
Status write_exact(Storage& storage, std::uint64_t offset, ByteView input);

}  // namespace tfdb

#endif  // TFDB_STORAGE_HPP

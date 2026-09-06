// Copyright (c) 2026 Alexander Safronenko
// SPDX-License-Identifier: BSD-2-Clause

#ifndef TFDB_CODEC_HPP
#define TFDB_CODEC_HPP

#include <cstdint>
#include <memory>
#include <vector>

#include "tfdb/bytes.hpp"
#include "tfdb/status.hpp"

namespace tfdb {

enum class CompressionId : std::uint16_t {
  none = 0,
  packbits = 1,
  lz4_block = 2  // Reserved; not implemented by the dependency-free core.
};

class CompressionCodec {
 public:
  virtual ~CompressionCodec() {}
  virtual CompressionId id() const = 0;
  virtual std::uint16_t version() const = 0;
  virtual std::size_t max_compressed_size(std::size_t input_size) const = 0;
  // Input may view the current contents of *output. Implementations must
  // consume it before mutating output (a temporary vector + swap is simple).
  // Methods used by AsyncWriter must report errors, not throw.
  virtual Status compress(ByteView input,
                          std::vector<std::uint8_t>* output) const = 0;
  virtual Status decompress(ByteView input, std::size_t expected_size,
                            std::vector<std::uint8_t>* output) const = 0;
};

std::shared_ptr<const CompressionCodec> no_compression_codec();
std::shared_ptr<const CompressionCodec> packbits_codec();
std::shared_ptr<const CompressionCodec> built_in_codec(CompressionId id);

}  // namespace tfdb

#endif  // TFDB_CODEC_HPP

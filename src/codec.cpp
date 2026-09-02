#include "tfdb/codec.hpp"

#include <algorithm>
#include <limits>

namespace tfdb {
namespace {

class NoneCodec final : public CompressionCodec {
 public:
  CompressionId id() const override { return CompressionId::none; }
  std::uint16_t version() const override { return 1; }
  std::size_t max_compressed_size(std::size_t input_size) const override {
    return input_size;
  }
  Status compress(ByteView input, std::vector<std::uint8_t>* output) const override {
    if (!output) return Status::Error(StatusCode::invalid_argument, "null output");
    if (!input.valid())
      return Status::Error(StatusCode::invalid_argument,
                           "null non-empty compression input");
    std::vector<std::uint8_t> encoded;
    if (!input.empty()) encoded.assign(input.data(), input.data() + input.size());
    output->swap(encoded);
    return Status::Ok();
  }
  Status decompress(ByteView input, std::size_t expected_size,
                    std::vector<std::uint8_t>* output) const override {
    if (!output) return Status::Error(StatusCode::invalid_argument, "null output");
    if (!input.valid())
      return Status::Error(StatusCode::invalid_argument,
                           "null non-empty decompression input");
    if (input.size() != expected_size) {
      return Status::Error(StatusCode::corrupt, "raw block size mismatch");
    }
    std::vector<std::uint8_t> decoded;
    if (!input.empty()) decoded.assign(input.data(), input.data() + input.size());
    output->swap(decoded);
    return Status::Ok();
  }
};

// TFDB PackBits v1:
//   token 0..127   => token+1 literal bytes follow
//   token 128..255 => (token&127)+3 copies of one following byte
// Runs of three or more bytes use repeat form. Literal groups are at most 128.
class PackBitsCodec final : public CompressionCodec {
 public:
  CompressionId id() const override { return CompressionId::packbits; }
  std::uint16_t version() const override { return 1; }
  std::size_t max_compressed_size(std::size_t input_size) const override {
    const std::size_t overhead = input_size / 128 +
                                 (input_size % 128 == 0 ? 0 : 1);
    if (input_size > std::numeric_limits<std::size_t>::max() - overhead)
      return std::numeric_limits<std::size_t>::max();
    return input_size + overhead;
  }

  Status compress(ByteView input, std::vector<std::uint8_t>* output) const override {
    if (!output) return Status::Error(StatusCode::invalid_argument, "null output");
    if (!input.valid())
      return Status::Error(StatusCode::invalid_argument,
                           "null non-empty compression input");
    std::vector<std::uint8_t> encoded;
    encoded.reserve(max_compressed_size(input.size()));
    std::size_t position = 0;
    while (position < input.size()) {
      std::size_t run = repeated(input, position);
      if (run >= 3) {
        run = std::min<std::size_t>(run, 130);
        encoded.push_back(static_cast<std::uint8_t>(0x80u | (run - 3u)));
        encoded.push_back(input.data()[position]);
        position += run;
        continue;
      }
      const std::size_t literal_begin = position;
      position += run;
      while (position < input.size() && position - literal_begin < 128) {
        run = repeated(input, position);
        if (run >= 3) break;
        position += std::min<std::size_t>(run, 128 - (position - literal_begin));
      }
      const std::size_t count = position - literal_begin;
      encoded.push_back(static_cast<std::uint8_t>(count - 1));
      encoded.insert(encoded.end(), input.data() + literal_begin,
                     input.data() + position);
    }
    output->swap(encoded);
    return Status::Ok();
  }

  Status decompress(ByteView input, std::size_t expected_size,
                    std::vector<std::uint8_t>* output) const override {
    if (!output) return Status::Error(StatusCode::invalid_argument, "null output");
    if (!input.valid())
      return Status::Error(StatusCode::invalid_argument,
                           "null non-empty decompression input");
    std::vector<std::uint8_t> decoded;
    decoded.reserve(expected_size);
    std::size_t position = 0;
    while (position < input.size()) {
      const std::uint8_t token = input.data()[position++];
      if ((token & 0x80u) == 0) {
        const std::size_t count = static_cast<std::size_t>(token) + 1;
        if (count > input.size() - position ||
            count > expected_size - decoded.size()) {
          return Status::Error(StatusCode::corrupt, "invalid PackBits literal");
        }
        decoded.insert(decoded.end(), input.data() + position,
                       input.data() + position + count);
        position += count;
      } else {
        const std::size_t count = static_cast<std::size_t>(token & 0x7fu) + 3;
        if (position == input.size() || count > expected_size - decoded.size()) {
          return Status::Error(StatusCode::corrupt, "invalid PackBits run");
        }
        decoded.insert(decoded.end(), count, input.data()[position++]);
      }
    }
    if (decoded.size() != expected_size) {
      return Status::Error(StatusCode::corrupt, "PackBits output size mismatch");
    }
    output->swap(decoded);
    return Status::Ok();
  }

 private:
  static std::size_t repeated(ByteView input, std::size_t position) {
    std::size_t count = 1;
    while (position + count < input.size() && count < 130 &&
           input.data()[position + count] == input.data()[position]) {
      ++count;
    }
    return count;
  }
};

}  // namespace

std::shared_ptr<const CompressionCodec> no_compression_codec() {
  static std::shared_ptr<const CompressionCodec> codec(new NoneCodec());
  return codec;
}

std::shared_ptr<const CompressionCodec> packbits_codec() {
  static std::shared_ptr<const CompressionCodec> codec(new PackBitsCodec());
  return codec;
}

std::shared_ptr<const CompressionCodec> built_in_codec(CompressionId id) {
  if (id == CompressionId::none) return no_compression_codec();
  if (id == CompressionId::packbits) return packbits_codec();
  return std::shared_ptr<const CompressionCodec>();
}

}  // namespace tfdb

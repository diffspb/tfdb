// Copyright (c) 2026 Alexander Safronenko
// SPDX-License-Identifier: BSD-2-Clause

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

// TFDB LZ4 block v1 is the LZ4 raw block format carried directly in a TFDB
// block payload. It has no frame, magic, or length prefix of its own: the
// block header already supplies the stored size, the raw size, and the CRC32C
// that is verified before this decoder runs. docs/format-v1.md section 6
// states the normative grammar.
//
// The encoder is deliberately a single hash table with no match chains and no
// lazy matching. Any valid LZ4 stream decodes identically everywhere, so
// encoder quality only moves the compression ratio, while decoder correctness
// is a data-loss question. Everything the search depends on is a byte value or
// an input offset, never an address or a clock, so one input always produces
// one output on every supported platform.
constexpr std::size_t kLz4MinMatch = 4;
// The last five bytes of a block are always literals and the last match starts
// at least twelve bytes before the end. Both are LZ4 encoder rules that let a
// conforming decoder copy in wide steps; TFDB honors them so its media stays
// readable by stock LZ4 decoders.
constexpr std::size_t kLz4LastLiterals = 5;
constexpr std::size_t kLz4MatchFindLimit = 12;
constexpr std::size_t kLz4MaxDistance = 65535;
constexpr unsigned kLz4HashLog = 12;
// A stored byte buys at most 255 output bytes, because that is the most an
// extended-length byte can add. Used only to bound a reservation, never to
// classify a stream.
constexpr std::size_t kLz4MaxExpansion = 255;

// Explicit little-endian load. Reading the host word order here would make the
// encoder's match search, and therefore its output bytes, depend on the
// platform.
std::uint32_t lz4_load32(const std::uint8_t* data, std::size_t position) {
  return static_cast<std::uint32_t>(data[position]) |
         (static_cast<std::uint32_t>(data[position + 1]) << 8) |
         (static_cast<std::uint32_t>(data[position + 2]) << 16) |
         (static_cast<std::uint32_t>(data[position + 3]) << 24);
}

std::size_t lz4_hash(std::uint32_t sequence) {
  const std::uint32_t scattered = static_cast<std::uint32_t>(
      sequence * 2654435761u);
  return static_cast<std::size_t>(scattered >> (32 - kLz4HashLog));
}

void lz4_write_length(std::size_t remaining, std::vector<std::uint8_t>* out) {
  while (remaining >= 255) {
    out->push_back(255);
    remaining -= 255;
  }
  out->push_back(static_cast<std::uint8_t>(remaining));
}

void lz4_append_literals(const std::uint8_t* data, std::size_t begin,
                         std::size_t count, std::vector<std::uint8_t>* out) {
  // data may legitimately be null for an empty input, and null + 0 is not
  // worth relying on.
  if (count != 0) out->insert(out->end(), data + begin, data + begin + count);
}

// Literals, a two-byte little-endian offset, and a match length biased by the
// four-byte minimum. The token nibbles carry the two lengths up to 14 each and
// escape to 255-terminated byte runs beyond that.
void lz4_emit_sequence(const std::uint8_t* data, std::size_t literal_begin,
                       std::size_t literal_count, std::size_t distance,
                       std::size_t match_length,
                       std::vector<std::uint8_t>* out) {
  const std::size_t token_index = out->size();
  out->push_back(0);
  std::uint8_t token = 0;
  if (literal_count >= 15) {
    token = 0xf0u;
    lz4_write_length(literal_count - 15, out);
  } else {
    token = static_cast<std::uint8_t>(literal_count << 4);
  }
  lz4_append_literals(data, literal_begin, literal_count, out);
  out->push_back(static_cast<std::uint8_t>(distance & 0xffu));
  out->push_back(static_cast<std::uint8_t>((distance >> 8) & 0xffu));
  const std::size_t encoded_match = match_length - kLz4MinMatch;
  if (encoded_match >= 15) {
    token = static_cast<std::uint8_t>(token | 0x0fu);
    lz4_write_length(encoded_match - 15, out);
  } else {
    token = static_cast<std::uint8_t>(token | encoded_match);
  }
  (*out)[token_index] = token;
}

// The final sequence is literals only: no offset follows and the token's
// match nibble stays zero.
void lz4_emit_last_literals(const std::uint8_t* data, std::size_t literal_begin,
                            std::size_t literal_count,
                            std::vector<std::uint8_t>* out) {
  const std::size_t token_index = out->size();
  out->push_back(0);
  if (literal_count >= 15) {
    (*out)[token_index] = 0xf0u;
    lz4_write_length(literal_count - 15, out);
  } else {
    (*out)[token_index] = static_cast<std::uint8_t>(literal_count << 4);
  }
  lz4_append_literals(data, literal_begin, literal_count, out);
}

// Continues a saturated nibble. The accumulator is capped by the output size
// the block header already declared, so neither the running total nor the loop
// can be driven anywhere by the stored bytes.
bool lz4_read_length(const std::uint8_t* stored, std::size_t stored_size,
                     std::size_t limit, std::size_t* position,
                     std::size_t* length) {
  for (;;) {
    if (*position == stored_size) return false;
    const std::uint8_t extra = stored[(*position)++];
    if (*length > limit || extra > limit - *length) return false;
    *length += extra;
    if (extra != 255) return true;
  }
}

std::size_t lz4_output_reserve(std::size_t stored_size,
                               std::size_t expected_size) {
  if (stored_size >
      (std::numeric_limits<std::size_t>::max() - 32) / kLz4MaxExpansion)
    return expected_size;
  const std::size_t reachable = stored_size * kLz4MaxExpansion + 32;
  return reachable < expected_size ? reachable : expected_size;
}

class Lz4BlockCodec final : public CompressionCodec {
 public:
  CompressionId id() const override { return CompressionId::lz4_block; }
  std::uint16_t version() const override { return 1; }
  std::size_t max_compressed_size(std::size_t input_size) const override {
    // One all-literal sequence: a token, one extended-length byte per 255
    // literals, then the literals themselves. The constant covers the token,
    // the first escape byte, and the empty-input encoding.
    const std::size_t overhead = input_size / 255 + 16;
    if (input_size > std::numeric_limits<std::size_t>::max() - overhead)
      return std::numeric_limits<std::size_t>::max();
    return input_size + overhead;
  }

  Status compress(ByteView input,
                  std::vector<std::uint8_t>* output) const override {
    if (!output) return Status::Error(StatusCode::invalid_argument, "null output");
    if (!input.valid())
      return Status::Error(StatusCode::invalid_argument,
                           "null non-empty compression input");
    const std::uint8_t* const data = input.data();
    const std::size_t size = input.size();
    std::vector<std::uint8_t> encoded;
    encoded.reserve(max_compressed_size(size));
    std::size_t anchor = 0;
    // Below thirteen bytes no match may start at all, so the whole input is
    // one literal run and the table is never needed.
    if (size > kLz4MatchFindLimit) {
      // Positions are stored biased by one so that zero means "never seen".
      std::vector<std::size_t> table(std::size_t(1) << kLz4HashLog, 0);
      const std::size_t match_find_limit = size - kLz4MatchFindLimit;
      const std::size_t match_limit = size - kLz4LastLiterals;
      std::size_t position = 0;
      while (position < match_find_limit) {
        const std::size_t slot = lz4_hash(lz4_load32(data, position));
        const std::size_t candidate = table[slot];
        table[slot] = position + 1;
        if (candidate == 0) {
          ++position;
          continue;
        }
        // Positions are inserted in increasing order, so a recorded candidate
        // is always strictly behind the cursor and the distance is at least 1.
        const std::size_t match = candidate - 1;
        const std::size_t distance = position - match;
        if (distance > kLz4MaxDistance ||
            lz4_load32(data, match) != lz4_load32(data, position)) {
          ++position;
          continue;
        }
        std::size_t length = kLz4MinMatch;
        while (position + length < match_limit &&
               data[match + length] == data[position + length]) {
          ++length;
        }
        lz4_emit_sequence(data, anchor, position - anchor, distance, length,
                          &encoded);
        position += length;
        anchor = position;
      }
    }
    lz4_emit_last_literals(data, anchor, size - anchor, &encoded);
    output->swap(encoded);
    return Status::Ok();
  }

  Status decompress(ByteView input, std::size_t expected_size,
                    std::vector<std::uint8_t>* output) const override {
    if (!output) return Status::Error(StatusCode::invalid_argument, "null output");
    if (!input.valid())
      return Status::Error(StatusCode::invalid_argument,
                           "null non-empty decompression input");
    const std::uint8_t* const stored = input.data();
    const std::size_t stored_size = input.size();
    std::vector<std::uint8_t> decoded;
    decoded.reserve(lz4_output_reserve(stored_size, expected_size));
    std::size_t position = 0;
    // Where the last match landed in the output, for the end-of-block
    // parsing restrictions checked once the stream ends.
    bool matched = false;
    std::size_t last_match_begin = 0;
    std::size_t last_match_end = 0;
    while (position < stored_size) {
      const std::uint8_t token = stored[position++];
      std::size_t literals = static_cast<std::size_t>(token >> 4);
      if (literals == 15 && !lz4_read_length(stored, stored_size, expected_size,
                                             &position, &literals)) {
        return Status::Error(StatusCode::corrupt, "invalid LZ4 literal length");
      }
      if (literals > stored_size - position ||
          literals > expected_size - decoded.size()) {
        return Status::Error(StatusCode::corrupt,
                             "LZ4 literals exceed input or raw size");
      }
      decoded.insert(decoded.end(), stored + position,
                     stored + position + literals);
      position += literals;
      if (position == stored_size) {
        // The stream ends after literals. A match nibble here announces a
        // match whose offset was never stored.
        if ((token & 0x0fu) != 0)
          return Status::Error(StatusCode::corrupt,
                               "LZ4 final sequence announces a match");
        break;
      }
      if (stored_size - position < 2)
        return Status::Error(StatusCode::corrupt, "truncated LZ4 match offset");
      const std::size_t distance =
          static_cast<std::size_t>(stored[position]) |
          (static_cast<std::size_t>(stored[position + 1]) << 8);
      position += 2;
      if (distance == 0 || distance > decoded.size())
        return Status::Error(StatusCode::corrupt,
                             "LZ4 match offset outside decoded output");
      std::size_t match = static_cast<std::size_t>(token & 0x0fu);
      if (match == 15 && !lz4_read_length(stored, stored_size, expected_size,
                                          &position, &match)) {
        return Status::Error(StatusCode::corrupt, "invalid LZ4 match length");
      }
      if (match > std::numeric_limits<std::size_t>::max() - kLz4MinMatch)
        return Status::Error(StatusCode::corrupt, "LZ4 match length overflow");
      match += kLz4MinMatch;
      if (match > expected_size - decoded.size())
        return Status::Error(StatusCode::corrupt, "LZ4 match exceeds raw size");
      const std::size_t begin = decoded.size();
      decoded.resize(begin + match);
      // Byte at a time on purpose: an offset below the match length repeats
      // the overlapping window, which is how LZ4 encodes byte and word runs.
      for (std::size_t i = 0; i != match; ++i)
        decoded[begin + i] = decoded[begin - distance + i];
      matched = true;
      last_match_begin = begin;
      last_match_end = decoded.size();
    }
    if (decoded.size() != expected_size)
      return Status::Error(StatusCode::corrupt, "LZ4 output size mismatch");
    // The LZ4 parsing restrictions: the last five bytes of a compressed block
    // are literals, and the last match starts at least twelve bytes before the
    // end of the block. Every conforming encoder honors them, so a stream that
    // does not is damaged or foreign, and accepting it would only widen what
    // corruption can turn into plausible output.
    if (matched && (expected_size - last_match_end < kLz4LastLiterals ||
                    expected_size - last_match_begin < kLz4MatchFindLimit)) {
      return Status::Error(StatusCode::corrupt,
                           "LZ4 last match breaks the parsing restrictions");
    }
    output->swap(decoded);
    return Status::Ok();
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

std::shared_ptr<const CompressionCodec> lz4_block_codec() {
  static std::shared_ptr<const CompressionCodec> codec(new Lz4BlockCodec());
  return codec;
}

std::shared_ptr<const CompressionCodec> built_in_codec(CompressionId id) {
  if (id == CompressionId::none) return no_compression_codec();
  if (id == CompressionId::packbits) return packbits_codec();
  if (id == CompressionId::lz4_block) return lz4_block_codec();
  return std::shared_ptr<const CompressionCodec>();
}

}  // namespace tfdb

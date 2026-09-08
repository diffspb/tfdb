// Copyright (c) 2026 Alexander Safronenko
// SPDX-License-Identifier: BSD-2-Clause

use crate::{Error, Result};

/// Decode the bounded TFDB PackBits v1 grammar.
pub fn decompress_packbits(stored: &[u8], raw_size: usize) -> Result<Vec<u8>> {
    let mut output = Vec::with_capacity(raw_size);
    let mut cursor = 0usize;
    while cursor < stored.len() {
        let token = stored[cursor];
        cursor += 1;
        if token <= 127 {
            let count = usize::from(token) + 1;
            let end = cursor
                .checked_add(count)
                .ok_or_else(|| Error::corrupt("PackBits literal length overflow"))?;
            if end > stored.len() || count > raw_size.saturating_sub(output.len()) {
                return Err(Error::corrupt("PackBits literal exceeds input or raw size"));
            }
            output.extend_from_slice(&stored[cursor..end]);
            cursor = end;
        } else {
            let count = usize::from(token & 127) + 3;
            if cursor == stored.len() || count > raw_size.saturating_sub(output.len()) {
                return Err(Error::corrupt("PackBits repeat exceeds input or raw size"));
            }
            let value = stored[cursor];
            cursor += 1;
            output.resize(output.len() + count, value);
        }
    }
    if output.len() != raw_size {
        return Err(Error::corrupt(
            "PackBits output size differs from block header",
        ));
    }
    Ok(output)
}

/// Bytes of match a saturated token nibble already implies.
const LZ4_MIN_MATCH: usize = 4;
/// One stored byte buys at most 255 output bytes, which is the most a single
/// extended-length byte can add. Used only to bound a reservation.
const LZ4_MAX_EXPANSION: usize = 255;
/// The last five bytes of a compressed block are literals, and the last match
/// starts at least twelve bytes before the end. Both are LZ4 encoder rules.
const LZ4_LAST_LITERALS: usize = 5;
const LZ4_MATCH_FIND_LIMIT: usize = 12;

/// Continue a saturated length nibble. The accumulator is capped by the output
/// size the block header already declared, so a run of `0xff` bytes can drive
/// neither the total nor the loop anywhere.
fn lz4_read_length(stored: &[u8], limit: usize, cursor: &mut usize, start: usize) -> Option<usize> {
    let mut length = start;
    loop {
        if *cursor == stored.len() {
            return None;
        }
        let extra = usize::from(stored[*cursor]);
        *cursor += 1;
        if length > limit || extra > limit - length {
            return None;
        }
        length += extra;
        if extra != 255 {
            return Some(length);
        }
    }
}

fn lz4_reserve(stored_size: usize, raw_size: usize) -> usize {
    match stored_size
        .checked_mul(LZ4_MAX_EXPANSION)
        .and_then(|reachable| reachable.checked_add(32))
    {
        Some(reachable) => reachable.min(raw_size),
        None => raw_size,
    }
}

/// Decode the bounded TFDB LZ4 block v1 grammar.
///
/// This is a decoder only; the Rust implementation never writes media. It is
/// bounded entirely by `raw_size` from the block header, follows the LZ4 raw
/// block format, and rejects the three cases `docs/format-v1.md` names where
/// TFDB is deliberately stricter than a permissive LZ4 decoder: a zero match
/// offset, a match announced by the final token, and an output that does not
/// end at exactly `raw_size`.
pub fn decompress_lz4_block(stored: &[u8], raw_size: usize) -> Result<Vec<u8>> {
    let mut output = Vec::with_capacity(lz4_reserve(stored.len(), raw_size));
    let mut cursor = 0usize;
    // Where the last match landed in the output, for the end-of-block parsing
    // restrictions checked once the stream ends.
    let mut last_match: Option<(usize, usize)> = None;
    while cursor < stored.len() {
        let token = stored[cursor];
        cursor += 1;
        let mut literals = usize::from(token >> 4);
        if literals == 15 {
            literals = lz4_read_length(stored, raw_size, &mut cursor, literals)
                .ok_or_else(|| Error::corrupt("invalid LZ4 literal length"))?;
        }
        if literals > stored.len() - cursor || literals > raw_size - output.len() {
            return Err(Error::corrupt("LZ4 literals exceed input or raw size"));
        }
        output.extend_from_slice(&stored[cursor..cursor + literals]);
        cursor += literals;
        if cursor == stored.len() {
            // The stream ends after literals. A match nibble here announces a
            // match whose offset was never stored.
            if token & 0x0f != 0 {
                return Err(Error::corrupt("LZ4 final sequence announces a match"));
            }
            break;
        }
        if stored.len() - cursor < 2 {
            return Err(Error::corrupt("truncated LZ4 match offset"));
        }
        let distance = usize::from(stored[cursor]) | (usize::from(stored[cursor + 1]) << 8);
        cursor += 2;
        if distance == 0 || distance > output.len() {
            return Err(Error::corrupt("LZ4 match offset outside decoded output"));
        }
        let mut length = usize::from(token & 0x0f);
        if length == 15 {
            length = lz4_read_length(stored, raw_size, &mut cursor, length)
                .ok_or_else(|| Error::corrupt("invalid LZ4 match length"))?;
        }
        length = length
            .checked_add(LZ4_MIN_MATCH)
            .ok_or_else(|| Error::corrupt("LZ4 match length overflow"))?;
        if length > raw_size - output.len() {
            return Err(Error::corrupt("LZ4 match exceeds raw size"));
        }
        let begin = output.len();
        output.resize(begin + length, 0);
        // Byte at a time on purpose: an offset below the match length repeats
        // the overlapping window, which is how LZ4 encodes byte and word runs.
        for i in 0..length {
            output[begin + i] = output[begin - distance + i];
        }
        last_match = Some((begin, output.len()));
    }
    if output.len() != raw_size {
        return Err(Error::corrupt("LZ4 output size differs from block header"));
    }
    // Every conforming encoder honors the parsing restrictions, so a stream
    // that does not is damaged or foreign, and accepting it would only widen
    // what corruption can turn into plausible output.
    if let Some((begin, end)) = last_match {
        if raw_size - end < LZ4_LAST_LITERALS || raw_size - begin < LZ4_MATCH_FIND_LIMIT {
            return Err(Error::corrupt(
                "LZ4 last match violates the block parsing restrictions",
            ));
        }
    }
    Ok(output)
}

#[cfg(test)]
mod tests {
    use super::{decompress_lz4_block, decompress_packbits};

    #[test]
    fn literals_and_runs() {
        assert_eq!(
            decompress_packbits(&[2, b'a', b'b', b'c', 128, b'x'], 6).unwrap(),
            b"abcxxx"
        );
    }

    #[test]
    fn malformed_input_is_rejected() {
        assert!(decompress_packbits(&[2, b'a'], 3).is_err());
        assert!(decompress_packbits(&[128], 3).is_err());
        assert!(decompress_packbits(&[128, b'x'], 2).is_err());
        assert!(decompress_packbits(&[], 1).is_err());
    }

    #[test]
    fn lz4_literals_matches_and_overlap() {
        // One literal, a distance-1 match of 14 bytes, then five last
        // literals: the normative twenty-byte run vector.
        assert_eq!(
            decompress_lz4_block(
                &[0x1a, b'a', 0x01, 0x00, 0x50, b'a', b'a', b'a', b'a', b'a'],
                20
            )
            .unwrap(),
            vec![b'a'; 20]
        );
        // Extended literal length: 15 + 5 literals, no match.
        let mut stream = vec![0xf0, 5];
        stream.extend_from_slice(&[b'z'; 20]);
        assert_eq!(decompress_lz4_block(&stream, 20).unwrap(), vec![b'z'; 20]);
        assert_eq!(decompress_lz4_block(&[0x00], 0).unwrap(), Vec::<u8>::new());
        assert_eq!(decompress_lz4_block(&[], 0).unwrap(), Vec::<u8>::new());
    }

    #[test]
    fn lz4_malformed_input_is_rejected() {
        let valid = [0x1a, b'a', 0x01, 0x00, 0x50, b'a', b'a', b'a', b'a', b'a'];
        assert!(decompress_lz4_block(&valid, 20).is_ok());
        // Zero offset: invalid per the LZ4 block format, and permissive
        // decoders turn it into plausible garbage instead of an error.
        let mut zero_offset = valid;
        zero_offset[2] = 0;
        assert!(decompress_lz4_block(&zero_offset, 20).is_err());
        // Offset reaching before the start of the decoded output.
        let mut far_offset = valid;
        far_offset[2] = 9;
        assert!(decompress_lz4_block(&far_offset, 20).is_err());
        // Final token announcing a match whose offset was never stored.
        let mut final_match = valid;
        final_match[4] = 0x51;
        assert!(decompress_lz4_block(&final_match, 20).is_err());
        // A stream that decodes to fewer or more bytes than the block header
        // declared, and every truncation of a valid stream.
        assert!(decompress_lz4_block(&valid, 19).is_err());
        assert!(decompress_lz4_block(&valid, 21).is_err());
        for length in 0..valid.len() {
            assert!(decompress_lz4_block(&valid[..length], 20).is_err());
        }
        // A match that ends inside the last five bytes, and one that starts
        // less than twelve bytes before the end: both violate the LZ4 parsing
        // restrictions that every conforming encoder honors.
        assert!(decompress_lz4_block(&[0x10, b'a', 0x01, 0x00, 0x00], 5).is_err());
        assert!(decompress_lz4_block(
            &[0x50, b'a', b'b', b'c', b'd', b'e', 0x01, 0x00, 0x50, b'v', b'w', b'x', b'y', b'z'],
            14
        )
        .is_err());
        // Literal and match runs that would exceed the declared raw size.
        assert!(decompress_lz4_block(&[0x20, b'a', b'b'], 1).is_err());
        assert!(decompress_lz4_block(&[0x1f, b'a', 1, 0, 0xff, 0xff, 0x00], 8).is_err());
        // An extended length that never terminates before the input does.
        assert!(decompress_lz4_block(&[0xf0, 0xff, 0xff], 4096).is_err());
    }
}

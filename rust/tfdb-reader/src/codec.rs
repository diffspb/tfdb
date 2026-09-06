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

#[cfg(test)]
mod tests {
    use super::decompress_packbits;

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
}

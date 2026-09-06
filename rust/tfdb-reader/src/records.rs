use crate::{crc32c, Error, Result};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FramedRecord<'a> {
    pub encoded: &'a [u8],
    pub payload: &'a [u8],
    pub index_time_ns: i64,
    pub selector: u64,
    pub flags: u16,
}

/// Validate and decode all concatenated FramedRecordV1 records in one block.
pub fn decode_framed_v1(block: &[u8]) -> Result<Vec<FramedRecord<'_>>> {
    let mut result = Vec::new();
    let mut offset = 0usize;
    while offset < block.len() {
        if block.len() - offset < 32 {
            return Err(Error::corrupt("truncated FramedRecordV1 header"));
        }
        let bytes = &block[offset..];
        let frame_size = u32::from_le_bytes(bytes[0..4].try_into().unwrap()) as usize;
        let header_size = u16::from_le_bytes(bytes[4..6].try_into().unwrap()) as usize;
        let payload_size = u32::from_le_bytes(bytes[24..28].try_into().unwrap()) as usize;
        let expected_size = header_size
            .checked_add(payload_size)
            .ok_or_else(|| Error::corrupt("FramedRecordV1 size overflow"))?;
        if header_size != 32 || frame_size != expected_size || frame_size > bytes.len() {
            return Err(Error::corrupt("invalid FramedRecordV1 bounds"));
        }
        let encoded = &bytes[..frame_size];
        let expected_crc = u32::from_le_bytes(encoded[28..32].try_into().unwrap());
        let mut checked = encoded.to_vec();
        checked[28..32].fill(0);
        if crc32c(&checked) != expected_crc {
            return Err(Error::corrupt("FramedRecordV1 CRC32C mismatch"));
        }
        result.push(FramedRecord {
            encoded,
            payload: &encoded[header_size..],
            index_time_ns: i64::from_le_bytes(encoded[8..16].try_into().unwrap()),
            selector: u64::from_le_bytes(encoded[16..24].try_into().unwrap()),
            flags: u16::from_le_bytes(encoded[6..8].try_into().unwrap()),
        });
        offset += frame_size;
    }
    Ok(result)
}

#[cfg(test)]
mod tests {
    use super::decode_framed_v1;

    fn hex(value: &str) -> Vec<u8> {
        value
            .as_bytes()
            .chunks_exact(2)
            .map(|pair| {
                let text = std::str::from_utf8(pair).unwrap();
                u8::from_str_radix(text, 16).unwrap()
            })
            .collect()
    }

    #[test]
    fn normative_record() {
        let bytes = hex("23000000200007000000000000000080887766554433221103000000e8c1198a6c6f67");
        let records = decode_framed_v1(&bytes).unwrap();
        assert_eq!(records.len(), 1);
        assert_eq!(records[0].index_time_ns, i64::MIN);
        assert_eq!(records[0].selector, 0x1122_3344_5566_7788);
        assert_eq!(records[0].flags, 7);
        assert_eq!(records[0].payload, b"log");
    }
}

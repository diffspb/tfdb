/// CRC32C (Castagnoli), reflected polynomial, as used by TFDB v1.
pub fn crc32c(bytes: &[u8]) -> u32 {
    let mut value = 0xffff_ffffu32;
    for &byte in bytes {
        value ^= u32::from(byte);
        for _ in 0..8 {
            value = (value >> 1) ^ if value & 1 != 0 { 0x82f6_3b78 } else { 0 };
        }
    }
    value ^ 0xffff_ffffu32
}

#[cfg(test)]
mod tests {
    #[test]
    fn known_vector() {
        assert_eq!(super::crc32c(b"123456789"), 0xe306_9283);
    }
}

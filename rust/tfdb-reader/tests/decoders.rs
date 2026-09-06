// Copyright (c) 2026 Alexander Safronenko
// SPDX-License-Identifier: BSD-2-Clause

use std::fs;
use std::path::PathBuf;

use tfdb_reader::{
    decode_block_header, decode_footer, decode_framed_v1, decode_index_entry,
    decode_partition_header, decode_volume_header, decompress_packbits, Event, Reader,
};

fn corpus() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../testdata/format-v1/valid-mixed.tfdb")
}

fn every_truncation_is_rejected(bytes: &[u8], accepts: impl Fn(&[u8]) -> bool) {
    for length in 0..bytes.len() {
        assert!(!accepts(&bytes[..length]), "accepted length {length}");
    }
}

fn every_single_bit_mutation_is_rejected(bytes: &[u8], accepts: impl Fn(&[u8]) -> bool) {
    for offset in 0..bytes.len() {
        for bit in 0..8 {
            let mut mutated = bytes.to_vec();
            mutated[offset] ^= 1 << bit;
            assert!(!accepts(&mutated), "accepted byte {offset} bit {bit}");
        }
    }
}

#[test]
fn all_persistent_structures_reject_truncation() {
    let volume = fs::read(corpus()).unwrap();
    every_truncation_is_rejected(&volume[0..256], |bytes| decode_volume_header(bytes).is_ok());
    every_truncation_is_rejected(&volume[8192..8192 + 1024], |bytes| {
        decode_partition_header(bytes).is_ok()
    });
    every_truncation_is_rejected(&volume[12288..12288 + 128], |bytes| {
        decode_block_header(bytes).is_ok()
    });
    every_truncation_is_rejected(&volume[65536..65536 + 48], |bytes| {
        decode_index_entry(bytes).is_ok()
    });
    every_truncation_is_rejected(&volume[69632..69632 + 256], |bytes| {
        decode_footer(bytes).is_ok()
    });
}

#[test]
fn crc_protected_structures_and_records_reject_every_single_bit_mutation() {
    let volume = fs::read(corpus()).unwrap();
    every_single_bit_mutation_is_rejected(&volume[0..256], |bytes| {
        decode_volume_header(bytes).is_ok()
    });
    every_single_bit_mutation_is_rejected(&volume[8192..8192 + 1024], |bytes| {
        decode_partition_header(bytes).is_ok()
    });
    every_single_bit_mutation_is_rejected(&volume[12288..12288 + 128], |bytes| {
        decode_block_header(bytes).is_ok()
    });
    every_single_bit_mutation_is_rejected(&volume[69632..69632 + 256], |bytes| {
        decode_footer(bytes).is_ok()
    });

    let reader = Reader::open(corpus()).unwrap();
    let mut first_block = None;
    reader
        .scan_blocks(|event| {
            if let Event::Data(block) = event {
                first_block = Some(block.data);
                return false;
            }
            true
        })
        .unwrap();
    every_single_bit_mutation_is_rejected(&first_block.unwrap(), |bytes| {
        decode_framed_v1(bytes).is_ok()
    });
}

#[test]
fn arbitrary_bounded_inputs_do_not_panic() {
    let mut state = 0xdec0_de12_3456_789au64;
    for trial in 0..10_000usize {
        let size = trial % 1200;
        let mut bytes = vec![0u8; size];
        for byte in &mut bytes {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            *byte = state as u8;
        }
        let result = std::panic::catch_unwind(|| {
            let _ = decode_volume_header(&bytes);
            let _ = decode_partition_header(&bytes);
            let _ = decode_block_header(&bytes);
            let _ = decode_index_entry(&bytes);
            let _ = decode_footer(&bytes);
            let _ = decode_framed_v1(&bytes);
            let _ = decompress_packbits(&bytes, trial % 512);
        });
        assert!(result.is_ok(), "decoder panicked on trial {trial}");
    }
}

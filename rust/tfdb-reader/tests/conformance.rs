// Copyright (c) 2026 Alexander Safronenko
// SPDX-License-Identifier: BSD-2-Clause

use std::path::PathBuf;

use tfdb_reader::{decode_framed_v1, ErrorKind, Event, Reader, FRAMED_RECORD_V1_PROFILE_ID};

fn corpus() -> PathBuf {
    corpus_case("valid-mixed")
}

fn corpus_case(name: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../testdata/format-v1")
        .join(format!("{name}.tfdb"))
}

fn open_error(name: &str) -> ErrorKind {
    match Reader::open(corpus_case(name)) {
        Ok(_) => panic!("{name} unexpectedly opened"),
        Err(error) => error.kind(),
    }
}

fn homogeneous_blocks(name: &str) -> Vec<(u64, u32, u8)> {
    let reader = Reader::open(corpus_case(name)).unwrap();
    assert!(reader.initial_gaps().is_empty());
    let mut result = Vec::new();
    reader
        .scan_blocks(|event| {
            let Event::Data(block) = event else {
                panic!("unexpected gap in {name}");
            };
            assert_eq!(block.data.len(), 200);
            assert!(block.data.iter().all(|byte| *byte == block.data[0]));
            result.push((
                block.metadata.partition_generation,
                block.metadata.block_sequence,
                block.data[0],
            ));
            true
        })
        .unwrap();
    result
}

#[test]
fn reads_cpp_written_candidate_volume() {
    let reader = Reader::open(corpus()).unwrap();
    let volume = reader.volume();
    assert_eq!(volume.volume_id_high, 0x0102_0304_0506_0708);
    assert_eq!(volume.volume_id_low, 0x1112_1314_1516_1718);
    assert_eq!(volume.partition_count, 2);
    assert_eq!(reader.initial_gaps(), []);

    let partitions = reader.partitions();
    assert_eq!(partitions.len(), 2);
    assert!(partitions[0].sealed);
    assert_eq!(partitions[0].compression_id, 1);
    assert_eq!(partitions[0].time_domain_id, 42);
    assert!(!partitions[1].sealed);
    assert_eq!(partitions[1].compression_id, 0);
    assert_eq!(partitions[1].time_domain_id, 43);

    let mut blocks = 0usize;
    let mut records = 0usize;
    let mut raw_bytes = 0usize;
    reader
        .scan_blocks(|event| {
            let Event::Data(block) = event else {
                panic!("unexpected conformance gap");
            };
            assert_eq!(block.metadata.record_format_id, FRAMED_RECORD_V1_PROFILE_ID);
            let decoded = decode_framed_v1(&block.data).unwrap();
            assert_eq!(decoded.len(), block.metadata.record_count as usize);
            blocks += 1;
            records += decoded.len();
            raw_bytes += block.data.len();
            true
        })
        .unwrap();
    assert_eq!((blocks, records, raw_bytes), (4, 8, 714));
}

#[test]
fn query_keeps_physical_order_and_filters_only_block_ranges() {
    let reader = Reader::open(corpus()).unwrap();
    let mut sequences = Vec::new();
    reader
        .query_blocks(
            tfdb_reader::TimeRange {
                begin_ns: 950,
                end_ns: 1700,
                time_domain_id: 42,
            },
            |event| {
                if let Event::Data(block) = event {
                    sequences.push(block.metadata.block_sequence);
                }
                true
            },
        )
        .unwrap();
    assert_eq!(sequences, vec![1, 2]);
}

#[test]
fn shared_feature_and_bounds_cases_have_documented_classes() {
    let optional = Reader::open(corpus_case("feature-optional-metadata")).unwrap();
    assert_eq!(optional.partitions().len(), 2);
    assert_eq!(
        open_error("feature-required-unknown"),
        ErrorKind::Unsupported
    );
    assert_eq!(
        open_error("feature-optional-region"),
        ErrorKind::Unsupported
    );
    assert_eq!(
        open_error("bounds-volume-partition-size"),
        ErrorKind::Corrupt
    );

    let overflow = Reader::open(corpus_case("bounds-feature-region-overflow")).unwrap();
    assert_eq!(overflow.initial_gaps().len(), 1);
    assert_eq!(overflow.partitions().len(), 1);
}

#[test]
fn shared_stale_writer_chain_excludes_the_stale_suffix() {
    assert_eq!(
        homogeneous_blocks("stale-writer-chain"),
        vec![(1, 0, b'A'), (1, 1, b'D')]
    );
}

#[test]
fn shared_live_rotation_snapshots_show_reuse_without_pinning() {
    assert_eq!(
        homogeneous_blocks("live-rotation-01-before-reuse"),
        vec![(1, 0, b'A'), (2, 0, b'B')]
    );
    assert_eq!(
        homogeneous_blocks("live-rotation-02-header-reused"),
        vec![(2, 0, b'B')]
    );
    assert_eq!(
        homogeneous_blocks("live-rotation-03-new-block"),
        vec![(2, 0, b'B'), (3, 0, b'C')]
    );
}

/// The `lz4_block:1` corpus image. The Rust reader has no encoder, so this is
/// the only way its decoder is held to the exact bytes the C++ writer chose:
/// an extended literal run, extended and short match lengths, an overlapping
/// distance-one run, and one block that fell back to `none` because the codec
/// could not shrink it.
#[test]
fn shared_lz4_block_partition_decodes_to_the_written_payloads() {
    let reader = Reader::open(corpus_case("codec-lz4-block")).unwrap();
    assert!(reader.initial_gaps().is_empty());
    let partitions = reader.partitions();
    assert_eq!(partitions.len(), 1);
    assert_eq!(partitions[0].compression_id, 2);
    assert_eq!(partitions[0].compression_version, 1);

    let run = vec![0x2au8; 2000];
    let mut extended: Vec<u8> = (0..40u32).map(|i| (i * 7 + 1) as u8).collect();
    extended.extend((0..1200u32).map(|i| (i % 4) as u8));
    let cycles: Vec<u8> = (0..1500u32).map(|i| ((i % 16) + (i / 64)) as u8).collect();
    let mut state = 0x1234567u32;
    let noise: Vec<u8> = (0..1500)
        .map(|_| {
            state = state.wrapping_mul(1103515245).wrapping_add(12345);
            (state >> 16) as u8
        })
        .collect();
    let expected = [run, extended, cycles, noise];

    let mut blocks = Vec::new();
    reader
        .scan_blocks(|event| {
            let Event::Data(block) = event else {
                panic!("unexpected gap in codec-lz4-block");
            };
            blocks.push((block.metadata.compression_id, block.data));
            true
        })
        .unwrap();
    assert_eq!(blocks.len(), 4);
    for (index, payload) in expected.iter().enumerate() {
        assert_eq!(&blocks[index].1, payload, "block {index}");
    }
    // Three blocks shrank and kept the partition codec; the pseudo-random one
    // could not and stores itself uncompressed inside the same partition.
    assert_eq!(
        blocks.iter().map(|block| block.0).collect::<Vec<_>>(),
        vec![2, 2, 2, 0]
    );

    // The same volume claiming lz4_block:2 is unsupported at open. A reader
    // that assumed version 1 was close enough would decode the blocks into
    // whatever version 2 happens not to mean.
    assert_eq!(open_error("codec-unknown-version"), ErrorKind::Unsupported);
}

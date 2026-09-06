use std::path::PathBuf;

use tfdb_reader::{decode_framed_v1, Event, Reader, FRAMED_RECORD_V1_PROFILE_ID};

fn corpus() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../testdata/format-v1/valid-mixed.tfdb")
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

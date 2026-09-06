use std::fs::{self, OpenOptions};
use std::os::unix::fs::FileExt;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};

use tfdb_reader::{ErrorKind, Event, Reader};

static NEXT_FILE: AtomicU64 = AtomicU64::new(0);

struct Scratch(PathBuf);

impl Scratch {
    fn copy() -> Self {
        let source = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../testdata/format-v1/valid-mixed.tfdb");
        let ordinal = NEXT_FILE.fetch_add(1, Ordering::Relaxed);
        let destination = std::env::temp_dir().join(format!(
            "tfdb-rust-recovery-{}-{ordinal}.tfdb",
            std::process::id(),
        ));
        fs::copy(source, &destination).unwrap();
        Self(destination)
    }

    fn flip(&self, offset: u64) {
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .open(&self.0)
            .unwrap();
        let mut byte = [0u8; 1];
        assert_eq!(file.read_at(&mut byte, offset).unwrap(), 1);
        byte[0] ^= 1;
        assert_eq!(file.write_at(&byte, offset).unwrap(), 1);
        file.sync_all().unwrap();
    }
}

impl Drop for Scratch {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.0);
    }
}

fn counts(reader: &Reader) -> (usize, usize) {
    let mut blocks = 0usize;
    let mut gaps = 0usize;
    reader
        .scan_blocks(|event| {
            match event {
                Event::Data(_) => blocks += 1,
                Event::Gap(_) => gaps += 1,
            }
            true
        })
        .unwrap();
    (blocks, gaps)
}

#[test]
fn one_valid_volume_header_copy_is_sufficient() {
    let scratch = Scratch::copy();
    scratch.flip(0);
    assert_eq!(counts(&Reader::open(&scratch.0).unwrap()), (4, 0));
}

#[test]
fn two_invalid_volume_header_copies_are_rejected() {
    let scratch = Scratch::copy();
    scratch.flip(0);
    scratch.flip(4096);
    match Reader::open(&scratch.0) {
        Ok(_) => panic!("both damaged volume headers were accepted"),
        Err(error) => assert_eq!(error.kind(), ErrorKind::Corrupt),
    }
}

#[test]
fn corrupt_sealed_index_is_rebuilt_and_reported() {
    let scratch = Scratch::copy();
    // slot 0 base (8192) + index begin (65536 - 4096 - 4096).
    scratch.flip(65536);
    assert_eq!(counts(&Reader::open(&scratch.0).unwrap()), (4, 1));
}

#[test]
fn corrupt_sealed_payload_is_a_gap_without_hiding_other_blocks() {
    let scratch = Scratch::copy();
    // slot 0 base + first frame offset + 128-byte block header.
    scratch.flip(8192 + 4096 + 128);
    assert_eq!(counts(&Reader::open(&scratch.0).unwrap()), (3, 1));
}

#[test]
fn torn_active_tail_recovers_the_previous_complete_prefix() {
    let scratch = Scratch::copy();
    // slot 1 base + first frame header. The active partition then has no
    // complete block, while the older sealed partition remains readable.
    scratch.flip(8192 + 65536 + 4096);
    assert_eq!(counts(&Reader::open(&scratch.0).unwrap()), (3, 0));
}

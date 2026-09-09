# Independent Rust reader

`rust/tfdb-reader` is a Linux, read-only implementation of the TFDB media
format v1 candidate. It does not link to the C++ library, include its internal
headers, use third-party crates, or permit `unsafe` code. Its purpose is
interoperability review and offline/live read access; it is not a second
writer.

The implementation proves that the specification can be interpreted from
fixed-width little-endian bytes independently. It does **not** by itself freeze
format v1. The remaining freeze gates are tracked in [`roadmap.md`](roadmap.md).

## Capabilities

The crate validates and exposes:

- redundant volume headers and complete geometry checks;
- partition identities, generations, feature directories, and time domains;
- sealed indexes with CRC32C, sequence/span checks, and footer summaries;
- footerless recovery by the longest complete block prefix;
- block/header/payload CRC32C and writer-incarnation chains during scans;
- uncompressed, TFDB PackBits v1, and TFDB LZ4 block v1 blocks;
- physical-order scans and time-domain/range candidate-block queries;
- overwrite and corruption gaps as explicit events;
- FramedRecordV1 sizes, CRC, timestamp, selector, flags, and payload.

The reader accepts a regular file or seekable Linux block device. For a device
wrapper that cannot seek to its end, call `Reader::from_file_with_size()` with
an independently established size. It never writes to the descriptor.

Only built-in compression IDs `none:1`, `packbits:1`, and `lz4_block:1` are
supported. A partition or block requiring another codec or another version of
these is `Unsupported`; it is never guessed. Unknown application record
profiles remain opaque at the block API.

The crate decodes but never encodes, so its LZ4 implementation is a decoder
only. It follows the grammar in `docs/format-v1.md` section 6, including the
cases TFDB rejects that a permissive LZ4 decoder would accept: a zero match
offset, a match announced by the final token, a last match that breaks the LZ4
parsing restrictions, and any output that does not end at exactly `raw_size`.
The shared corpus image `codec-lz4-block.tfdb` is what holds this decoder to
the exact bytes the C++ encoder chose.

## Build and test

The crate supports Rust 1.70 or later and has no crate dependencies:

```sh
cargo build --manifest-path rust/tfdb-reader/Cargo.toml --release
cargo test --manifest-path rust/tfdb-reader/Cargo.toml
cargo fmt --manifest-path rust/tfdb-reader/Cargo.toml -- --check
```

From the repository root, `make rust-test` runs its unit, conformance, and
recovery tests. `make conformance` regenerates the shared C++ volume and checks
both readers against every case in
`testdata/format-v1/manifest.tsv`.

## Library API

Open and inspect without loading all indexes:

```rust
use tfdb_reader::Reader;

let reader = Reader::open("telemetry.tfdb")?;
println!("partitions={}", reader.volume().partition_count);
for partition in reader.partitions() {
    println!("generation={} blocks={}",
             partition.generation, partition.block_count);
}
# Ok::<(), tfdb_reader::Error>(())
```

Scan decoded blocks in physical order. Every callback receives owned bytes, so
the block can safely outlive the call if the application moves it elsewhere:

```rust
use tfdb_reader::{Event, Reader};

let reader = Reader::open("telemetry.tfdb")?;
reader.scan_blocks(|event| {
    match event {
        Event::Data(block) => consume(block.metadata, block.data),
        Event::Gap(gap) => report_gap(gap),
    }
    true // false stops normally
})?;
# Ok::<(), tfdb_reader::Error>(())
```

`query_blocks(TimeRange, visitor)` applies the v1 block-envelope test in one
nonzero time domain. It does not sort results and can return false-positive
blocks when records inside one block are disordered. Decode FramedRecordV1 and
apply exact record filtering above it:

```rust
use tfdb_reader::{decode_framed_v1, Event, Reader, TimeRange};

let reader = Reader::open("telemetry.tfdb")?;
reader.query_blocks(TimeRange {
    begin_ns: 1_000,
    end_ns: 2_000,
    time_domain_id: 1,
}, |event| {
    if let Event::Data(block) = event {
        match decode_framed_v1(&block.data) {
            Ok(records) => for record in records {
                if (1_000..2_000).contains(&record.index_time_ns) {
                    consume_record(record);
                }
            },
            Err(error) => report_decode_error(error),
        }
    }
    true
})?;
# Ok::<(), tfdb_reader::Error>(())
```

Applications must still compare the decoded record count with
`block.metadata.record_count`; `tfdb-rs-verify` performs that check for the
built-in framed profile.

The primary error classes are `InvalidArgument`, `Io`, `Corrupt`,
`Unsupported`, and `Overwritten`. Diagnostic message text is not a persistent
or cross-language ABI. Gaps are delivered as events so callers can continue
extracting later physical blocks while recording incomplete output.

Generate detailed local API pages with:

```sh
cargo doc --manifest-path rust/tfdb-reader/Cargo.toml --no-deps --open
```

Omit `--open` on headless or build machines.

## Command-line tools

Release binaries are written below `rust/tfdb-reader/target/release/`:

```sh
tfdb-rs-inspect STORE
tfdb-rs-verify STORE
tfdb-rs-dump STORE
tfdb-rs-dump STORE --framed-v1 --from BEGIN --to END \
  --time-domain DOMAIN --selector SELECTOR
tfdb-rs-dump STORE --raw-blocks > blocks.bin
```

Exit code 0 means complete success, 2 means open/argument/I/O/unsupported
failure, and 3 means extraction completed with one or more explicit gaps.
Raw-block output is a sequence of little-endian `u32` lengths followed by
decoded block bytes, matching the C++ tool.

## Concurrency boundary

The reader snapshots partition generations at open. Before and after payload
I/O it re-reads partition identity; detected reuse is an overwrite gap. A slow
reader does not pin rotation. As with the C++ reader, another process that
writes outside TFDB's single-writer protocol receives no guarantee.

The current 20-case shared corpus checks byte-for-byte C++ regeneration, valid
PackBits/raw/LZ4 partitions, both time domains, disordered and unsynchronized
time, redundant-header recovery, invalid index rebuild, payload corruption,
torn active tail, damaged partition metadata, unsupported format major,
truncation, unknown feature handling, checked-arithmetic bounds, stale writer
incarnations, static snapshots across live rotation, an `lz4_block:1`
partition whose blocks cover extended literal and match lengths, an
overlapping distance-one run, and a payload the codec could not shrink, which
therefore stores itself as `none`, and the same partition claiming
`lz4_block:2`, which both readers must call unsupported at open. A concurrent
long-running rotation soak and hardware durability remain separate gates.

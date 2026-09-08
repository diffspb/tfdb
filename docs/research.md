# Research notes and borrowed ideas

This document records the external designs considered for TFDB. They are
references, not dependencies. TFDB remains a purpose-built circular store
because none of the reviewed formats combines fixed media size, deterministic
rotation, low metadata write amplification, bounded recovery, and opaque
project records.

## MCAP

MCAP is the closest record/chunk reference. Its chunks carry earliest/latest
log time, uncompressed size, compression identity, and optional CRC. Message
Index records follow their Chunk in the Data section; the Summary contains
Chunk Index records that point to them. Records are length framed and future
record types are designed to be skipped.

Ideas retained:

- independently decompressible chunks;
- explicit stored and uncompressed lengths;
- per-chunk min/max time rather than assuming message order;
- codec identifiers and versions in persistent metadata;
- checksums around independently recoverable units;
- a compact summary/index separated from bulk data.

Not retained: MCAP is a growing archival file finalized with a footer. It does
not define fixed-size circular reuse, partition generations, durability
checkpoints, or behavior when a reader races an overwrite.

Source: [MCAP format specification](https://mcap.dev/spec).

## systemd journal

The journal file format stores an entry sequence number together with
realtime and monotonic timestamps; monotonic time is associated with a boot
identifier. This is the important lesson for machines that can begin near the
Unix epoch and synchronize later: a wall clock alone cannot reconstruct a
timeline.

TFDB keeps physical generation/block/record order authoritative. The core time
index uses the application-provided index timestamp and never rewrites it.
Applications that require post-facto clock reconstruction should include a
monotonic timestamp and boot/session identifier in their record codec. Making
those fields mandatory in the block layer would impose substantial overhead
on the very small telemetry records that dominate the target workload.

Source: [systemd Journal File Format](https://systemd.io/JOURNAL_FILE_FORMAT/).

## SQLite atomic commit

SQLite deliberately does not assume sector writes are atomic and uses sync
points and redundant state to distinguish old, new, and incomplete updates.
That invalidates the tempting assumption that one 8--32 KiB `write()` is an
atomic block commit.

Ideas retained:

- never require atomic multi-sector writes;
- validate every newly appended structural unit;
- order sync points around state transitions;
- recover from either the old generation or a completely validated new one.

TFDB does not use a rollback journal because data is immutable after append and
old ring generations are disposable. A valid prefix is identified by the full
set of block invariants: CRCs and bounds, volume/partition generation,
contiguous sequence, and the per-writer incarnation chain. The chain is what
prevents a CRC-valid stale same-generation suffix from reappearing after a
second crash.

Source: [Atomic Commit in SQLite](https://sqlite.org/atomiccommit.html).

## littlefs

littlefs combines append-style metadata, revision counts, redundancy, CRCs,
copy-on-write, bounded memory, bad-block handling, and dynamic wear leveling
for raw microcontroller flash. Its filesystem tree and erase/program management
solve a much broader and lower-level problem.

Ideas retained:

- generation/revision selects the newest complete instance;
- CRC is the inexpensive evidence that an append completed;
- bounded recovery and RAM are design constraints, not later optimizations;
- frequently rewritten global metadata is avoided.

Not retained: metadata pairs, allocation trees, and raw erase/program handling
belong below TFDB when Linux exposes eMMC/NAND through a controller/FTL. This
does not mean littlefs is an FTL; it owns those concerns because it directly
targets raw flash.

Sources: [littlefs design](https://github.com/littlefs-project/littlefs/blob/master/DESIGN.md)
and [on-disk specification](https://github.com/littlefs-project/littlefs/blob/master/SPEC.md).

## RocksDB WAL

RocksDB separates normal tail recovery, strict consistency, point-in-time
recovery, and best-effort salvage. Its WAL records and data blocks use
checksums, while flush policy controls the durability/throughput trade-off.

Ideas retained:

- tail corruption and interior corruption have different meaning;
- normal open stops at the first invalid active-tail block;
- verification and salvage are explicit tool modes rather than silent normal
  behavior;
- the durability contract names which call makes data durable.

Not retained: WAL plus memtable/SST compaction duplicates data and creates much
more write amplification than an append-only telemetry ring needs.

Sources: [RocksDB WAL recovery modes](https://github.com/facebook/rocksdb/wiki/WAL-Recovery-Modes)
and [checksum behavior](https://github.com/facebook/rocksdb/wiki/Basic-Operations#checksums).

## Zephyr FCB and NVS

Zephyr's Flash Circular Buffer is the closest small raw-flash analogue. It
stores FIFO entries containing a length, data, and checksum; an append is
finished by writing its checksum, and rotation erases the oldest flash sector.
Zephyr NVS is also circular, but it preserves the newest value per 16-bit key
and copies live values during sector reuse.

Ideas retained:

- circular FIFO reuse at a coarse erase/partition unit;
- a checksum marks a complete append;
- whole oldest units are abandoned to make progress;
- endurance must be calculated from write rate, record overhead, capacity, and
  conservative erase-cycle assumptions.

Not retained: TFDB runs above Linux files/block devices and therefore does not
erase raw sectors. NVS's latest-value semantics and copying are wrong for a
telemetry history, while FCB lacks compressed block indexing, Linux flush
semantics, and reader/generation race rules.

Sources: [Zephyr FCB](https://docs.zephyrproject.org/latest/services/storage/fcb/fcb.html)
and [Zephyr NVS](https://docs.zephyrproject.org/latest/services/storage/nvs/nvs.html).

## Common Trace Format, LTTng, and rosbag2

CTF demonstrates the value and cost of keeping schemas separate from binary
event streams. rosbag2's explicit serialization and storage plugin layers,
with both MCAP and SQLite storage, likewise show that a schema layer and a
storage layer should not be conflated.

CTF packet headers/context make independently usable packets and time bounds
explicit. LTTng overwrite mode uses fixed sub-buffers and sequence numbers so a
reader can report loss in flight-recorder operation. These validate TFDB's
coarse block/partition boundaries and explicit gap events.

TFDB therefore stores an opaque `record_format_id` and version in each
partition. The core can return blocks without knowing a project schema. A
parser/debug tool may register the matching record codec to split, filter, and
display records.

Sources: [Common Trace Format](https://diamon.org/ctf/),
[LTTng overwrite mode](https://lttng.org/docs/v2.16/), and
[rosbag2 storage plugin architecture](https://github.com/ros2/rosbag2/blob/rolling/README.md#storage-format-plugin-architecture).

## Linux durability interface

Linux and storage devices may buffer and reorder writes. `fdatasync()` waits
for file data and required metadata, while device write-back caches require
working flush support in the driver/controller path. `sync_file_range()` is
not a durability primitive and does not flush volatile device caches.

TFDB uses `fdatasync()` for checkpoints and exposes errors. A successful call
is the software boundary of its guarantee; hardware that acknowledges a flush
before non-volatile persistence cannot be repaired by an on-media format.

The first backend remains buffered. Linux documents that `O_DIRECT` alignment
depends on filesystem, kernel, and device, and that `O_DIRECT` alone is not a
durability guarantee. Requiring it would complicate the simple file/block API
without removing the need for a flush. The logical persistence quantum instead
separates frame starts and can be tuned independently.

For regular files, filesystem allocation and journaling add physical writes
that are absent from host `pwrite` counters; ext4's default mode protects
metadata ordering but does not turn application data writes into atomic
transactions. Raw block devices avoid filesystem metadata but increase
provisioning risk. The same media format is used, while endurance and power-cut
results are qualified separately.

Sources: [fsync/fdatasync manual](https://man7.org/linux/man-pages/man2/fdatasync.2.html),
[Linux block write-back cache control](https://kernel.org/doc/html/latest/block/writeback_cache_control.html),
[O_DIRECT rules](https://man7.org/linux/man-pages/man2/open.2.html),
[ext4 journal design](https://www.kernel.org/doc/html/latest/filesystems/ext4/journal.html),
and [sync_file_range warning](https://man7.org/linux/man-pages/man2/sync_file_range.2.html).

## Compression

Independent block compression is compatible with random access and bounded
memory. A standard codec is preferable to inventing a complex format. LZ4's
block format is byte-oriented, independently specified, and fast, but it does
not carry its own compressed or decompressed sizes. TFDB would have to bound
both from its block header and output buffer; even a small correct encoder is
non-trivial code.

Format v1 therefore makes compression a versioned block codec. `none` is
mandatory. The dependency-free implementation supplies a simple PackBits-style
byte-run codec and an LZ4 raw block codec. A writer stores compressed bytes
only when they are smaller, so incompressible input is never expanded on media.

PackBits is intentionally a baseline, not a claim of best compression. It is
small enough to exhaustively test and useful for long repeated-byte runs such
as zero-filled fields. Ordinary repeated text still contains too few adjacent
equal bytes and, like mixed binary telemetry, may gain little.

The first real consumer showed exactly that limit. Compressing its sixty
independent chunks the way TFDB compresses blocks, PackBits recovered 0.8% of
743 kB while `lz4_block:1` recovered 15.0%, because the payload is already
entropy-coded and has almost no adjacent equal bytes left for a byte-run codec
to find. zstd-3 would recover 21% on the same data; LZ4 takes about 70% of that
without the dependency, which is why ID 2 was implemented and zstd was not.
Neither number generalizes: TFDB stores opaque application records, and an
application writing already-compressed payloads gains nothing, which is why the
codec stays optional and per-partition.

TFDB implements the LZ4 block format rather than vendoring the reference,
because the reference encoder hashes five bytes on 64-bit builds and four on
32-bit ones and so does not emit byte-identical output across platforms, which
the byte-exact shared corpus requires. The implementation is checked against
liblz4 in both directions instead.

The immutable codec registry lets a project qualify another byte-specified
codec without entangling the ring logic. A codec becomes part of the
cross-language standard only with bounded decompression, malformed-input tests,
and C++/Rust golden vectors.

Source: [LZ4 block format](https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md).

## Synthesis

The common pattern across the mature systems is small immutable units with
lengths, identities, generations/sequence numbers, checksums, and explicit
commit boundaries. TFDB applies that pattern twice:

1. data blocks are the independently validated append units;
2. partition footer/index is the optional acceleration summary, rebuildable
   from data blocks.

The ring adds one rule absent from archival formats: every read must revalidate
the partition generation before exposing data, because an old physical offset
can become valid data from a newer generation while the reader is running.

## What the measurements can and cannot prove

The exact backend mock proves host call counts, byte ranges, flush ordering,
and recovery against the modeled torn/reordered persistence states. File
benchmarks prove throughput and host-request amplification on their recorded
machine. Neither reveals an eMMC controller's internal write amplification,
erase count, cache honesty, ECC margin, or behavior at end of life. Those need
vendor endurance data, device health telemetry where available, and repeated
physical power cuts on the production hardware/software stack.

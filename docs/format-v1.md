# TFDB media format v1 (candidate)

This document is the normative description of the C++ implementation's media
format. All multibyte integers are unsigned little-endian unless a field is
explicitly `i64`. Signed values use the two's-complement bit pattern. Structure
padding from a C or C++ compiler is never stored.

Format v1 is still a release candidate. The byte vectors in
`format_v1_normative_golden_vectors` and the shared corpus under
`testdata/format-v1/` are normative together with this document. The
independent Rust reader validates the current valid/corrupt/unsupported corpus;
the format becomes stable only after all remaining qualification gates in
[`roadmap.md`](roadmap.md) are complete.

## 1. Terminology and invariants

- A volume is one fixed-size regular file or Linux block device.
- The volume prefix is followed by `partition_count` equal-size slots.
- A partition incarnation is identified by the tuple `(volume_id, generation,
  slot)`.
- A block frame is immutable after it is written.
- A record is an opaque, nonempty, complete byte string passed in one
  `append()` call. Records never cross block boundaries.
- The physical order is generation, block sequence, then record position.
- `persistence_quantum` is a logical frame-start separation. It is not a claim
  about the NAND page, eMMC erase group, filesystem block, or `O_DIRECT`
  alignment.
- All byte ranges are checked without wrapping arithmetic before allocation or
  I/O.

The 128-bit `volume_id` is a format-incarnation identifier generated with
Linux `getrandom()`. It is not a stable vehicle/project identifier. Stable
identity belongs in application records or provisioning metadata.

## 2. Address space

```text
0                         volume header copy A (256 bytes in a 4096-byte slot)
4096                      volume header copy B (256 bytes in a 4096-byte slot)
8192                      partition slot 0
8192 + partition_size     partition slot 1
...
```

Trailing backend bytes that do not form a complete partition are unused.
There must be at least two complete partitions.

For a partition of size `P`, index reserve `I`, and footer region 4096:

```text
0                         partition header structure (1024 bytes)
0..4095                   partition header region
4096..index_begin-1       sequential block frames
index_begin=P-4096-I      sealed time index
footer_begin=P-4096       partition footer structure (256 bytes)
```

The writer may leave unused bytes in all reserved regions and between the end
of a frame and its aligned `frame_span`. Readers must not interpret those
bytes.

## 3. Common structure prefix

The volume, partition, block, and footer structures begin with:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | structure-specific ASCII magic |
| 8 | 2 | format major, `1` |
| 10 | 2 | format minor, `0` |
| 12 | 4 | exact encoded structure size |

A v1.0 reader rejects another major, a minor greater than it implements, and
an encoded size different from the exact v1.0 size. Prefix-compatible larger
structures are deliberately not guessed. A future specification must define
their CRC coverage and compatibility rules first.

Every structural CRC is CRC32C (Castagnoli), reflected polynomial
`0x82f63b78`, initial state `0xffffffff`, and final XOR `0xffffffff`. It covers
the entire encoded structure after treating its own four-byte field as zero.
The check value for ASCII `123456789` is `0xe3069283`.

## 4. Volume header, 256 bytes

Magic is `TFDBVOL1`. Copies are stored at offsets 0 and 4096.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 16 | common prefix |
| 16 | 8 | `volume_id_high` |
| 24 | 8 | `volume_id_low` |
| 32 | 8 | backend size recorded at format |
| 40 | 8 | partition size |
| 48 | 4 | partition count |
| 52 | 4 | index region size per partition |
| 56 | 4 | maximum raw block payload |
| 60 | 4 | persistence quantum |
| 64 | 8 | creation time supplied by application, `i64` ns |
| 72 | 4 | header CRC32C |
| 76 | 180 | zero in v1.0 |

Both valid copies must be byte-semantically identical. If one copy is invalid,
the other is sufficient. Two valid disagreeing copies are corruption, not a
tie to resolve heuristically.

Reader validity requirements are exact:

- the 128-bit volume ID is not all zero;
- recorded backend size is at least 8192 and no greater than the opened
  backend; partition count is at least two;
- quantum is a nonzero power of two no greater than 4096; 8192, 4096,
  partition size, and index size are all multiples of it;
- maximum raw block payload is nonzero and index size is at least 48;
- `fixed = 4096 header + index size + 4096 footer` is less than partition
  size, and the remaining data region is at least one quantum;
- `partition_count × partition_size` fits after the 8192-byte prefix without
  overflow or exceeding recorded backend size;
- `ceil((128 + maximum raw block payload) / quantum) × quantum` fits in the
  data region without overflow.

The canonical writer records the complete opened backend size and sets
partition count to `floor((backend_size - 8192) / partition_size)`; a trailing
remainder is unused. Every valid current-volume partition has a nonzero
generation, a slot number matching its physical slot, matching volume
geometry/ID, and a generation unique among all other valid current-volume
partitions.

## 5. Partition header, 1024 bytes

Magic is `TFDBPAR1`. The header is written once for a new generation and made
durable before any record is accepted into that partition.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 16 | common prefix |
| 16 | 8 | `volume_id_high` |
| 24 | 8 | `volume_id_low` |
| 32 | 8 | generation; zero is invalid |
| 40 | 4 | physical partition slot |
| 44 | 4 | zero |
| 48 | 8 | partition creation time, `i64`; zero in the v1 writer |
| 56 | 4 | maximum raw block payload |
| 60 | 4 | persistence quantum |
| 64 | 8 | allowed backward skew, `i64`; negative disables |
| 72 | 8 | allowed forward step, `i64`; negative disables |
| 80 | 8 | time-domain ID; zero is invalid on media and reserved for physical-scan APIs |
| 88 | 2 | feature count, exactly 4 for the v1 writer |
| 90 | 2 | feature descriptor size, 40 |
| 92 | 4 | feature directory offset, 128 |
| 96 | 4 | configuration area offset, 512 |
| 100 | 4 | configuration area length, 16 |
| 104 | 4 | partition-header CRC32C |
| 108 | 20 | zero |
| 128 | 320 | capacity for eight feature descriptors |
| 448 | 64 | unused by the v1 writer |
| 512 | 8 | record-format ID |
| 520 | 504 | unused by the v1 writer |

### Feature descriptor, 40 bytes

| Relative offset | Size | Field |
|---:|---:|---|
| 0 | 2 | feature kind |
| 2 | 2 | flags; bit 0 means required |
| 4 | 4 | algorithm ID |
| 8 | 2 | algorithm version |
| 10 | 2 | zero |
| 12 | 4 | config offset in partition header |
| 16 | 4 | config length |
| 20 | 4 | zero |
| 24 | 8 | reserved region offset in partition |
| 32 | 8 | reserved region length |

The four v1 descriptors are:

| Kind | Required | Algorithm | Version | Configuration/region |
|---:|:---:|---:|---:|---|
| 1 compression | yes | `0` none, `1` TFDB PackBits, `2` TFDB LZ4 block | 1 | no region/config |
| 2 time index | yes | `1` block min/max | 1 | the configured index tail region |
| 3 record profile | no | 0 | profile version | eight-byte profile ID at 512 |
| 4 integrity | yes | `1` CRC32C | 1 | no region/config |

The record-profile descriptor is structurally present even for opaque records
(`ID=0`, version 0). Its required bit is clear because block-level copying and
verification do not require a record parser.

Profile ID and version must either both be zero or both be nonzero. A mixed
zero/nonzero pair is corruption.

The v1 writer emits descriptor offset 128 and configuration area `[512,528)`.
A v1 reader requires those exact global offsets and lengths. It accepts four to
eight descriptors so an unknown optional feature can be skipped. Every
non-empty configuration slice must lie wholly inside `[512,528)`, configuration
slices must not overlap, and an empty slice has offset zero. The record-profile
slice is exactly `[512,520)`. Bytes not referenced by a descriptor are ignored
but remain protected by the partition-header CRC; the canonical writer emits
them as zero.

Although the descriptor's generic algorithm field is `u32`, compression IDs
in v1 are restricted to `0..65535` because the block header and public registry
use `u16`. A larger value is corruption, not a truncating cast.

An unknown required feature is unsupported. An unknown optional metadata-only
feature can be ignored. A v1 reader rejects an unknown feature that reserves a
media region because it cannot safely resume an active writer without knowing
that region's write rules. Structural bounds are checked before applying that
last semantic rule: a reserved-region range that overflows or extends outside
the partition is corruption, not merely an unsupported feature. Default
read-only open reports such damage in a current-volume partition header as a
gap under the normal partition-header recovery rule.

Partition-local options are immutable. New options take effect only after a
new partition header is durable; no buffered block crosses that boundary.

## 6. Block frame

### Block header, 128 bytes

Magic is `TFDBBLK1`.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 16 | common prefix |
| 16 | 8 | partition generation |
| 24 | 4 | block sequence, starting at zero |
| 28 | 4 | aggregate block flags |
| 32 | 4 | complete record count, nonzero |
| 36 | 4 | stored payload size, nonzero |
| 40 | 4 | raw payload size, nonzero |
| 44 | 4 | aligned frame span |
| 48 | 8 | minimum application index time, `i64` |
| 56 | 8 | maximum application index time, `i64` |
| 64 | 2 | compression ID |
| 66 | 2 | compression version |
| 68 | 4 | CRC32C of stored payload bytes |
| 72 | 4 | block-header CRC32C |
| 76 | 4 | partition slot |
| 80 | 8 | `volume_id_low` |
| 88 | 8 | `volume_id_high` |
| 96 | 8 | current writer-incarnation ID high |
| 104 | 8 | current writer-incarnation ID low |
| 112 | 8 | previous block's writer ID high; zero for sequence 0 |
| 120 | 8 | previous block's writer ID low; zero for sequence 0 |

The frame consists of the header immediately followed by `stored_size` bytes.
`frame_size = 128 + stored_size` and
`frame_span = align_up(frame_size, persistence_quantum)`. The writer issues one
logical contiguous write for the header and payload (normally one backend
`write_at()` call, with retries after successful short writes). This is an I/O
efficiency property, not an atomicity assumption.

For compression `none`, stored size equals raw size and version is 1. For a
configured codec, each block independently chooses compressed data only when
it is strictly smaller than raw data; otherwise the block identifies itself as
`none`. A decoder must produce exactly `raw_size` bytes.

Each writable open generates a nonzero random 128-bit writer-incarnation ID.
Within a partition, block zero names no predecessor; every later block names
the writer ID carried by the immediately preceding valid block. A recovered
writer may continue the same generation with a new current ID while linking
its first replacement block to the recovered prefix. A stale block from the
previous process cannot follow that replacement because its predecessor ID
does not match. This prevents a valid old suffix beyond a torn block from
becoming visible after a second crash, without sealing a mostly empty
partition at every vehicle restart.

### TFDB PackBits v1 byte grammar

The stored stream is a sequence of tokens and contains no terminator:

- token `0..127`: copy the following `token + 1` literal bytes;
- token `128..255`: copy the one following byte `(token & 127) + 3` times.

The decoder is bounded by the block header's `raw_size`. It rejects a missing
literal/repeat byte, any token that would exceed `raw_size`, and a final output
size unequal to `raw_size`. It accepts noncanonical but bounded streams. The
v1 encoder is canonical: runs of at least three bytes use repeat tokens, runs
are split at 130 bytes, and literal groups are split at 128 bytes or before the
next run of three. Empty codec input encodes empty, although a TFDB data block
itself is never empty. The writer uses PackBits bytes only when their length is
strictly less than raw input; otherwise the block codec is `none`.

### TFDB LZ4 block v1 byte grammar

The stored stream is the [LZ4 raw block
format](https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md) carried
directly in the block payload. TFDB deliberately takes the block format and not
the LZ4 frame format: a frame would repeat the magic, the flags, and the
decompressed size that the TFDB block header already carries, and it is a
little larger in practice. Everything the decoder needs comes from the block
header, which CRC32C has already validated: `stored_size` bounds the input and
`raw_size` bounds the output exactly.

The stream is a sequence of sequences and contains no terminator, no magic, and
no checksum of its own. Each sequence is:

- one token byte: the high nibble is a literal length, the low nibble a match
  length;
- if the literal nibble is `15`, additional bytes, each adding its value, the
  run ending at the first byte below `255`;
- that many literal bytes, copied to the output;
- a two-byte little-endian match offset, counted back from the current end of
  the output;
- if the match nibble is `15`, additional bytes read the same way;
- a match of `match nibble + 4` bytes copied from the offset. The copy is
  byte at a time: an offset smaller than the match length repeats the
  overlapping window, which is how a byte or word run is encoded.

The final sequence stops after its literals: no offset, no match.

A v1 decoder is bounded by `raw_size` and rejects, without reading or writing
outside either buffer:

- a literal run or match that would pass the end of the stored stream, or that
  the remaining `raw_size` cannot hold;
- an extended length whose `255` run reaches the end of the stored stream, or
  whose total already exceeds `raw_size`;
- a match offset of zero, or one larger than the number of bytes decoded so
  far;
- a final token whose match nibble is nonzero, which announces a match whose
  offset was never stored;
- a final output size other than `raw_size`.

Two of those are stricter than a permissive LZ4 decoder, deliberately. Offset
zero is invalid in the LZ4 specification, but a decoder that computes
`output - 0` copies bytes onto themselves and returns plausible garbage instead
of failing; TFDB treats damage that decodes as worse than damage that stops. A
nonzero match nibble on the final token is likewise a grammar violation that
permissive decoders ignore.

A v1 decoder also enforces the two LZ4 parsing restrictions, which every
conforming encoder honors, so any stream TFDB accepts a stock LZ4 decoder also
accepts: the last five bytes of the block are literals, and the last match
starts at least twelve bytes before the end. Both are checked against
`raw_size` once the stream ends and only when the stream contains a match.

The v1 encoder is canonical. It searches with a single 4096-entry hash table of
four-byte sequences, keeps no match chains and does no lazy matching, takes the
first candidate within the 65535-byte window whose four bytes match, and
extends it as far as the parsing restrictions allow. It emits a match only from
a position at least thirteen bytes before the end and never lets a match reach
into the last five bytes, so its output satisfies the restrictions above with a
byte to spare. Everything the search consults is a byte value or an input
offset, never an address, a clock, or the host word order, so one input yields
one byte-identical output on every supported platform and in every build.
Empty codec input encodes as the single token `0x00`, which is what a stock LZ4
encoder emits, although a TFDB data block itself is never empty. As with
PackBits, the writer uses LZ4 bytes only when their length is strictly less
than the raw input; otherwise the block codec is `none`, which is why a
partition configured for `lz4_block:1` may contain `none:1` blocks.

Normative vectors, raw input to stored stream:

| Raw | Stored |
|---|---|
| (empty) | `00` |
| `61` | `1061` |
| `61` × 13 | `d0` followed by `61` × 13 |
| `61` × 20 | `1a610100506161616161` |
| `000102`…`13` then `00010203` × 10 | `f005000102030405060708090a0b0c0d0e0f1011121314000f04000c500300010203` |

The third row is the largest input that cannot be compressed at all: below
thirteen bytes the parsing restrictions leave no position at which a match may
start. The fourth is one literal, a fourteen-byte match at offset one that
repeats the overlapping window, then the five mandatory last literals. The
fifth saturates both length nibbles.

Current flags are:

| Bit | Meaning |
|---:|---|
| 0 | one or more timestamps exceeded the configured anomaly threshold |
| 1 | one or more records were marked as using unsynchronized time |
| 16..31 | application-defined aggregate flags |

Bits 2..15 are reserved. The canonical writer leaves them zero; a v1 reader
does not reinterpret or discard them and exposes them as unknown aggregate
flags. Application input flags below bit 16 other than the defined
unsynchronized-time bit are not propagated by the v1 writer.

## 7. Time index entry, 48 bytes

Entries are stored without an individual CRC. The footer protects the complete
serialized index. Entry `N` describes block sequence `N`.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | block offset relative to partition |
| 8 | 4 | frame size |
| 12 | 4 | frame span |
| 16 | 4 | sequence |
| 20 | 4 | record count |
| 24 | 8 | minimum index time, `i64` |
| 32 | 8 | maximum index time, `i64` |
| 40 | 4 | block flags |
| 44 | 4 | raw size |

Entries must be contiguous by span from partition offset 4096 and must agree
with the corresponding CRC-protected block headers. The index is acceleration
data; if absent or invalid, the block sequence can be scanned and rebuilt.

## 8. Partition footer, 256 bytes

Magic is `TFDBFTR1`.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 16 | common prefix |
| 16 | 8 | `volume_id_high` |
| 24 | 8 | `volume_id_low` |
| 32 | 8 | generation |
| 40 | 4 | slot |
| 44 | 4 | aggregate partition flags |
| 48 | 4 | block count |
| 52 | 4 | index-entry size, 48 |
| 56 | 8 | index offset |
| 64 | 4 | index byte size, exactly count × 48 |
| 68 | 4 | zero |
| 72 | 8 | end of block spans relative to partition |
| 80 | 8 | partition minimum index time, `i64` |
| 88 | 8 | partition maximum index time, `i64` |
| 96 | 4 | CRC32C of serialized index bytes |
| 100 | 4 | footer CRC32C |
| 104 | 152 | zero |

A structurally valid footer identifies an attempted sealed partition. The
index CRC and block/index agreement are still checked before trusting the
index. For a nonempty partition, footer flags equal the bitwise OR of all block
flags and footer min/max times equal the extrema of all block ranges; all three
summary fields are zero for an empty partition. Strict open recomputes and
checks these summaries. A damaged index does not erase otherwise valid blocks.

## 9. Checkpoint, seal, and crash states

A normal checkpoint:

1. emits the current partial raw block, if any;
2. calls the backend flush operation;
3. advances durable record/block metrics only after success.

Sealing and rotation use this order:

1. emit a nonempty builder;
2. if data is dirty, flush it before writing any seal;
3. write the complete index;
4. write the footer;
5. flush index and footer;
6. write the next slot's new-generation partition header;
7. flush the new header before accepting its records.

The implementation never assumes that a 128-byte header, a frame, an index, or
a footer write is atomic. After an interruption, normal recovery accepts only
the longest block prefix whose complete headers, identities, sequence, bounds,
stored-payload CRCs, and codec ID/version/size rules validate. Normal scan does
not invoke the decompressor; strict open additionally decodes every retained
block and verifies the declared raw size.

The two flushes around the generation transition ensure that a durable new
header cannot precede durable old data and seal. A footer that reached media
without its whole index is detected by the index CRC and falls back to a block
scan.

## 10. Open and read rules

Open chooses the volume header, classifies every partition slot by full volume
ID, and orders valid current incarnations by generation. An empty or foreign-
volume slot is ignored. An unsupported required feature on the current volume
is an error. Default read-only open reports a damaged current-volume partition
header as a gap; strict open and every writable open reject it because a safe
next generation cannot be selected from ambiguous metadata.

A partition with no valid footer is scanned to its valid block prefix. More
than one such scan can be required after independent media corruption; only a
normal single-crash rotation path is expected to leave at most one active
partition.

A live reader snapshots the catalog but does not pin slots. It validates the
generation after reading or rebuilding a partition index. For every returned
block it:

1. verifies the current partition header and generation;
2. reads and validates block header and stored payload CRC;
3. reads the partition header and generation again;
4. decompresses into an owned temporary buffer;
5. invokes the visitor while that buffer is alive.

If reuse occurred, the reader emits an overwrite gap. Every apparent block
header, metadata, or payload-CRC failure is followed by a generation recheck
before it is classified as corruption. This closes the race where reuse begins
between the block-header and payload reads. The reader never silently joins
metadata and payload from different generations. A slow reader may lose data;
it cannot delay the writer.

## 11. Time semantics

Timestamps are application-provided signed nanoseconds in a partition-local
time domain. The core stores them unchanged. A block range is closed
`[min_time,max_time]`; a query is half-open `[begin,end)`. A block is a candidate
when `min_time < end && max_time >= begin`. Results remain in physical order.

Numerically different time domains are never compared. Domain zero is invalid
for a numerical time query; callers use `scan_blocks()` for an all-domain
physical scan. Clock repair, sorting, and wall-time reconstruction belong above
the block layer.

## 12. Optional FramedRecordV1 profile

Projects whose native messages are already self-framing can store them
directly. Other projects may use the optional 32-byte `FramedRecordV1` envelope:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | whole frame size |
| 4 | 2 | header size, 32 |
| 6 | 2 | record flags |
| 8 | 8 | index time, `i64` |
| 16 | 8 | selector/message/component ID |
| 24 | 4 | payload size |
| 28 | 4 | whole-frame CRC32C with this field zero |
| 32 | N | opaque payload |

All fields are little-endian. `header_size` must equal 32 and `whole frame
size` must equal `32 + payload_size` without overflow. Zero-length payloads are
valid. Frames are concatenated with no padding; the last frame must consume the
block exactly. A decoder rejects a remaining suffix shorter than 32 bytes, a
frame extending past the block, inconsistent sizes, or a CRC mismatch. The CRC
covers exactly `whole frame size` bytes with bytes 28..31 treated as zero.

Record flag bit 0 means the application time is unsynchronized and is also
aggregated into the containing block's unsynchronized-time flag. Bits 1..15
are project-defined and preserved; a generic v1 decoder does not reject them.

Its profile ID is `0x5446444252465631` and version is 1. The normative record
with time `INT64_MIN`, selector `0x1122334455667788`, flags 7, and payload ASCII
`log` is:

```text
23000000200007000000000000000080887766554433221103000000e8c1198a6c6f67
```

Exact record queries decode every selected candidate block, verify the record
count, and then apply time and selector filtering in physical order.

## 13. Compatibility registry

Persistent IDs are never reused. Current assignments are:

- compression 0: none v1;
- compression 1: TFDB PackBits v1;
- compression 2: TFDB LZ4 block v1, the LZ4 raw block format;
- time index 1: per-block signed min/max v1;
- integrity 1: CRC32C v1;
- record profile `0x5446444252465631`: FramedRecordV1.

A custom compression codec can be supplied by immutable registry at open, but
its ID/version and byte specification must be registered before media using it
is treated as portable. Unknown codec/version pairs are unsupported, never
guessed.

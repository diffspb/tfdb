# TFDB architecture

## 1. Goals and boundaries

TFDB stores a high-rate sequence of small telemetry messages or application log
records for years on eMMC/NAND-backed Linux systems. It is optimized for:

- sequential host writes and bounded metadata amplification;
- configurable loss of only the uncheckpointed tail after power failure;
- deterministic rotation inside a fixed-size file or block device;
- time-range block lookup with project-specific record filtering above it;
- bounded recovery work and bounded index RAM;
- a format implementable independently in C++14 and Rust;
- explicit diagnostics instead of silent repair or silent data substitution.

It is not a relational database, a filesystem, a globally time-sorted store, a
bad-block manager, or a distributed replication protocol. The Linux block
stack/controller is responsible for raw NAND management and wear levelling.

## 2. Layering

```text
application telemetry/log schema and parser
             | whole encoded record + index timestamp
             v
optional record codec/query adapter (split, identify, display, filter)
             |
             v
record block builder (whole-record packing, time statistics, anomaly flags)
             |
             v
block pipeline (compression, CRC, serialized block)
             |
             v
ring volume (partition generations, index/footer, recovery, rotation)
             |
             v
Storage backend (positional read/write/flush/size)
             |
             v
regular fixed-size file or Linux block device
```

This resolves the apparent circular dependency between block storage and
message parsing:

- the core sees the boundary of a record because each `append()` supplies one
  complete byte span, but it never interprets those bytes;
- the builder owns assembling records into a block and guarantees no record is
  split;
- the persistent block header carries only aggregate metadata required for
  recovery and time lookup;
- a record codec is needed only when a caller wants individual records rather
  than candidate blocks.

The lower block query remains useful even when an old project parser is no
longer installed: bytes can still be verified, copied, and preserved.

`RingStore::append()` is the trusted low-level seam: it cannot prove that
opaque bytes match the declared record profile. Profile-bound helpers such as
`append_framed_record()` check the active partition ID/version before writing.
Projects should expose typed helpers to normal application code and reserve raw
append for already validated native frames.

## 3. Volume and partition lifecycle

A volume has two CRC-protected static header copies followed by equal-sized
partitions. Static headers describe geometry and format version; they are not
updated on normal writes.

Each partition has:

1. a CRC-protected header written once when its generation begins;
2. variable-length block frames appended from the data start;
3. a fixed reserved tail region for time-index entries;
4. a footer slot written once after the index when the partition seals.

The logical lifecycle is `stale -> active -> sealed -> stale`. State is inferred
from validated artifacts rather than toggled in place:

- a matching header without a matching valid footer is active or interrupted;
- a matching header, index, and footer is sealed;
- blocks/footer with another generation are stale and ignored.

Only one partition is active. Its generation is greater than all retained
generations. A 64-bit generation cannot wrap within the product lifetime; a
wrap is treated as an unsupported/corrupt volume instead of adding subtle
modular ordering.

When the active data area or index capacity cannot accept another block:

1. flush any dirty data blocks before publishing a seal;
2. write the index into the reserved tail;
3. write the footer that describes the index;
4. `fdatasync()` the sealed partition state;
5. write the next physical partition header with generation + 1;
6. `fdatasync()` the new header before accepting its data.

A crash anywhere leaves either a recoverable active valid prefix, a sealed old
partition, or a validated new active header. No pointer update is required.

## 4. Blocks and compression

The builder holds at most one configured uncompressed block. `append()` either
adds the whole record or first emits the existing block. A record larger than
the block payload limit is rejected explicitly.

On emission, the compression codec runs once. Compressed representation is used
only if header plus compressed payload is smaller than the uncompressed form.
Encoded blocks have variable lengths. Frame starts are rounded up to a
configurable persistence quantum, so compression improves host bytes written
and can improve retention in quantum-sized steps. The gap is deliberately left
unwritten. No record is re-compressed after every append and no record crosses
a block boundary.

Each block header includes generation, sequence, encoded/raw lengths, record
count, min/max application time, flags, codec identity, and CRCs. A recovery
scan accepts the longest sequence of blocks with matching generation,
monotonic block sequence, bounded lengths, valid header, and valid payload.

Adjacent blocks also form a writer-incarnation chain. Every writable open gets
a random 128-bit ID; a block names both its current writer and the preceding
block's writer. After recovery a replacement block therefore breaks any stale
same-generation suffix beyond the torn point. This preserves the useful ability
to continue a large working partition across ordinary vehicle restarts; the
simpler alternative—seal and advance one whole partition on every open—was
rejected because frequent short trips could collapse retention.

CRC32C is mandatory in v1. It is not a claim that data is cryptographically
authentic; it is inexpensive evidence against torn writes, stale fragments,
and accidental corruption. A one-call `write()` is not assumed atomic.

## 5. Indexing and time

The application supplies an `int64` index timestamp with each record. TFDB does
not call realtime clocks on behalf of the application and does not modify the
value.

Input order may be slightly mixed. For each block the index stores the minimum
and maximum timestamp. A time query selects every block whose closed range
intersects the requested interval and returns selected blocks in physical
generation/block order. This can read false-positive records but cannot miss a
record merely because neighboring timestamps were out of order.

`query_blocks()` therefore returns candidate block bytes, not an exact record
answer. `query_records()` applies the single profile supplied by the caller,
verifies the decoded record count, and filters record timestamps and selectors
exactly. It returns `unsupported` when a candidate partition declares another
profile pair; mixed-profile history requires block-level application dispatch.
Neither API sorts by time.

The 64-bit selector in `FramedRecordV1` is a project-assigned lookup key. A
project may use the message ID when queries intentionally combine redundant
senders, or pack `(message_id, source_address)` when the two half-sets must be
distinguished. Schemas needing independent message and source predicates
should define both fields in their native record profile; v1 has no mandatory
secondary on-media index, so exact filtering happens after time-index block
selection.

A configurable delta threshold compares consecutive append timestamps. A
large forward jump or excessive regression marks the block `time_anomaly` and
increments diagnostics; the record remains stored and participates in min/max.
The flag is block-level because the core does not own the record schema. A
project codec may persist a per-record quality flag.

The physical tuple `(partition_generation, block_sequence, record_position)` is
the only clock-independent order. For systems that must reconstruct data
written near 1970, the recommended project envelope stores:

- application realtime;
- monotonic time;
- boot/session identifier;
- time-quality/synchronization event flags.

This mirrors a proven logging technique without charging every telemetry format
for fields it may not require. Approximate wall-time repair is a parser/export
operation and never mutates the original volume.

## 6. Active and sealed indexes

The writer builds the active partition index in RAM. Once a data block write
completes, it is published in the in-process catalog. Records still in the
builder's RAM block are deliberately invisible; exposing a few extra records
would require more synchronization for no durability benefit.

At seal, index entries are serialized to the reserved tail. If its fixed
capacity fills first, the partition seals early. Wasting remaining data bytes is
preferred to an overflow format or a second index structure.

Each query loads at most one sealed partition index at a time, then releases it
before moving to the next partition. Its size is bounded by the configured
fixed index reserve; concurrent readers multiply that bound. The writer holds
only the active index. A corrupt or absent index can be rebuilt by scanning data
blocks. The index is acceleration data, never the sole record of data
existence. Paging one very large partition index is a compatible future RAM
optimization, not an on-media change.

## 7. Durability contract

There are three distinct events:

- **accepted**: bytes were copied into the builder or async queue;
- **published**: a complete CRC-protected block was written through the backend;
- **durable**: the block was followed by a successful backend `flush`
  (`fdatasync()` for POSIX storage).

Only a durability checkpoint establishes the last guarantee. A checkpoint
emits a partial RAM block first, then flushes. Consequently, a background writer
that checkpoints at most every `T` seconds can bound durable-tail age only when
its queue drains at the qualified peak rate. The measured bound is checkpoint
schedule delay + queue residence + block publication + device completion. The
exact deployment claim must include worst-case measurements and the storage
controller's flush correctness.

The synchronous API exposes `checkpoint()` and `emergency_checkpoint()`; the
application must schedule them. The asynchronous wrapper owns a bounded queue,
single writer thread, periodic deadline, explicit barrier, and backpressure.
Its `submit_checked()` stores the expected record-profile/time-domain contract
with each queued record and revalidates it in the writer thread after any
automatic rotation. No background `sync_file_range()` is presented as
durability.

By default the async deadline and age metrics share `steady_clock`. Tests may
inject another monotonic nanosecond clock and wake the condition variable with
`notify_clock_advanced()`; this is a scheduling test seam, not persisted state
or an application-time source. Using one clock for both deadline and metrics
allows exact `interval + backend stall` assertions without sleeps. A custom
callback must be nonblocking, non-throwing, monotonic, and non-reentrant; any
clock-side mutex is released before notifying the writer. Timer expiration is
computed as `now >= start && now - start >= interval`, so a clock at
`UINT64_MAX` cannot create a saturated-deadline checkpoint loop. Default-clock
waits for very long intervals are split into one-hour chunks, avoiding overflow
inside implementations that calculate `steady_clock::now() + wait_for`. Queue
residence ends when an item is dequeued and therefore excludes block
publication and backend latency.

If a flush fails, the writer enters an error state and rejects further claims of
durability until reopened/recovered. Callers receive the original I/O error.

## 8. Recovery

Open performs bounded work:

1. choose a valid matching volume header copy;
2. read every small partition header/footer;
3. order retained partitions by generation;
4. validate sealed indexes lazily or, in strict mode, validate every indexed
   block and payload eagerly;
5. scan every current-volume partition lacking a valid footer to find its valid
   block prefix.

A normal single-crash transition leaves at most one unsealed partition. More
than one scan is possible after independent footer damage and is preferable to
silently dropping recoverable data.

An interrupted new header with no valid current-volume footer/block evidence is
ignored; the previous sealed generation remains authoritative and the next
partition can be started again. If a damaged header still has valid evidence
that the slot belonged to the current volume, lazy read-only open reports a
gap, strict open rejects it, and writable open refuses the ambiguous generation
order. An explicit salvage/reformat decision is required before overwriting
such a slot. If the active tail contains an invalid block, normal recovery
stops before it and may overwrite from that offset. It never searches past an
invalid tail for blocks that happen to look valid.

Interior corruption in an older sealed partition is reported. Normal reads can
either stop or report a gap according to query policy. A separate salvage tool
may scan for later block magic/generation/sequence candidates, but salvaged data
is labelled and never silently folded into normal results.

Default open validates structural metadata and defers sealed payload reads;
this minimizes startup I/O. Strict open verifies sealed indexes and every block
payload before success and rejects damaged current-volume headers. These are
integrity service levels, not different media formats.

## 9. Rotation and concurrent readers

One writer is allowed. Feeding it from multiple producers is outside the
library; this makes append order explicit and avoids hidden queues in the core.
Multiple in-process readers take immutable catalog snapshots and use positional
reads.

Readers do not pin partitions. A reader validates the generation after index
acquisition/rebuild, before each block, and after reading its payload. An
apparent header, metadata, or CRC failure triggers one more generation check:
slot reuse is reported as an `overwritten` gap, while damage in an unchanged
generation is reported as `corrupt`. The query continues or stops according to
policy and never returns a silent mixture of generations.

The POSIX backend takes a nonblocking advisory exclusive lock for a writer;
read-only file descriptors remain allowed. Other processes may open the file/device read-only. They receive the same
generation validation but no stable snapshot or lease. This is deliberate:
sequential storage is inspectable while live, but a slow external reader cannot
block data collection.

## 10. Extensibility and compatibility

All integers are explicitly encoded little-endian; C++ object layout is never
written directly. Every persistent structure has a magic, size, major/minor
version, reserved-zero bytes, bounds checks, and CRC.

The partition header contains a bounded feature directory. Each feature has a
kind, algorithm ID, version, required/optional flag, configuration slice, and
optional reserved media region. Unknown required features make the partition
unreadable. Unknown optional features may be skipped when the requested
operation does not depend on them.

Format changes follow these rules:

- the v1.0 reader requires exact v1.0 encoded sizes and rejects a newer minor;
- a future minor may add skippable information only after that minor defines
  exact size, CRC coverage, and compatibility behavior;
- semantic/incompatible changes require a new major;
- algorithm IDs are never reused;
- partition-local configuration allows new writes to change settings without
  migrating old partitions.

Golden byte vectors and corruption vectors are normative alongside the written
specification. The future Rust implementation must read C++ vectors and vice
versa.

The implemented extension seams are partition-local compression registries and
record profiles. The feature directory reserves a path for future index
plugins. A value index would receive block summaries from a project parser,
own a declared tail region, and remain rebuildable from records. It must not be
added as an unbounded callback inside the current writer: its maximum bytes,
recovery behavior, compatibility ID, and query false-positive rules need a
separate ADR and golden vectors first.

## 11. Logs

Application logs fit the block layer without special storage behavior. A log
codec should encode length, realtime, monotonic time, session/boot ID, severity,
component/channel, text/structured payload, and quality flags. `component` is
the natural message identifier for filtering; severity can later receive an
optional secondary index without altering the ring format.

Logs and telemetry may share a volume only when retention, privacy, and load
policies match. Separate partitions per type are not supported inside one ring;
separate volumes are simpler when log bursts must not evict safety telemetry.

Text logs often compress well with dictionary/match codecs such as LZ4 or Zstd,
but the v1 PackBits baseline only helps adjacent equal-byte runs and reduced
the reference log workload by only about 2%. Logs can also contain already-
compressed blobs or secrets. Compression is partition-local and optional.
Encryption and tamper authentication are intentionally outside format v1; CRC
is not a security mechanism.

The implemented `FramedRecordV1` is a practical envelope for text logs. A
dual-clock common log profile is intentionally deferred and analyzed in
[`logs.md`](logs.md).

## 12. Resource bounds

- writer RAM: one raw block, up to one compressed candidate, one contiguous
  frame, the active index, and the optional async queue;
- reader RAM: one serialized index buffer plus its decoded entries, one encoded
  block, one decoded block, and at most one retained footerless index copied
  from the catalog; older footerless partitions are rescanned lazily;
- open-recovery transient RAM: at most two decoded footerless indexes while a
  newly scanned generation replaces the previous candidate, independent of
  partition count;
- recovery I/O: all partition headers/footers plus at most one active data
  region in the normal case;
- steady metadata writes: one data block write per emitted block and one index
  plus footer at seal. Rotation of already-checkpointed data has two flushes
  (seal, new header); dirty rotation first checkpoints data and therefore has
  three. Periodic checkpoints add their configured flushes;
- low-level synchronous append copies into pre-reserved block storage without a
  per-record allocation; convenience profile helpers may allocate an envelope.

Exact bounds and write amplification are produced by the test/benchmark suite,
not asserted only from inspection.

## 13. File and block-device I/O

Both backends use fixed-offset `pread`/`pwrite` and `fdatasync`; neither relies
on a mutable file position. A regular file is preallocated and its parent
directory is synchronized when created. A block device is never resized or
formatted by filesystem operations.

The first implementation deliberately uses buffered I/O. `O_DIRECT` imposes
filesystem/device/version-specific buffer, length, and offset alignment and is
not itself a durability guarantee. The on-media persistence quantum therefore
does not force an application buffer alignment. A future direct-I/O backend can
be added behind `Storage` after it queries and satisfies platform alignment,
without changing the format.

The regular-file and raw-device code paths share the same logical recovery
rules, but they do not have identical physical amplification. A filesystem can
add allocation and journal traffic; a raw device exposes more provisioning
risk. Both require power-cut qualification of the complete kernel, driver,
controller, cache, and flash stack.

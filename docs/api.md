# TFDB C++14 API reference

TFDB (**Telemetry Flash DB**) exposes a dependency-free C++14 API under
`include/tfdb/`. This document describes the public candidate API, its
lifetime and durability contracts, and the intended production use of each
type. On-media details are specified separately in [`format-v1.md`](format-v1.md).

Start with [`tutorial.md`](tutorial.md) for an end-to-end integration.
The independent read-only Rust API is documented in
[`rust-reader.md`](rust-reader.md).

## API conventions

### Headers and namespace

All public names are in namespace `tfdb`:

| Header | Purpose |
|---|---|
| `tfdb/status.hpp` | status codes and diagnostics |
| `tfdb/bytes.hpp` | non-owning byte views |
| `tfdb/storage.hpp` | backend interface, POSIX backend, exact I/O helpers |
| `tfdb/codec.hpp` | block-compression interface and built-in codecs |
| `tfdb/ring_store.hpp` | volume, writer, block query, recovery, metrics |
| `tfdb/record_profile.hpp` | record decoding/filtering and FramedRecordV1 |
| `tfdb/async_writer.hpp` | bounded background writer |
| `tfdb/memory_storage.hpp` | deterministic in-memory/fault backend |
| `tfdb/counting_storage.hpp` | backend I/O counter decorator |

Headers under `src/`, including `internal_format.hpp`, are not public API.

### Error handling

Expected failures are returned as `Status`; TFDB does not use exceptions for
I/O or validation errors. Check every returned status. `Status::message()` is
diagnostic text, not a stable machine interface; branch on `StatusCode`.

Allocation and user-supplied callbacks/codecs use normal C++ semantics and may
still throw. Do not let exceptions escape across application ABI boundaries
that forbid them.

### Time and ranges

Indexed timestamps are signed 64-bit integer nanoseconds supplied by the
application. TFDB does not read realtime clocks for record indexing. A
`TimeRange` is half-open: `[begin_ns, end_ns)`. A nonzero `time_domain_id`
defines the meaning and epoch; `1` is Unix realtime by convention.

TFDB preserves physical append order. It does not sort records by timestamp.

### Ownership and borrowed views

`ByteView` and `MutableByteView` never own memory. In particular:

- `RingStore::append*()` copies the record before returning;
- `AsyncWriter::submit*()` copies the record before returning;
- `BlockEvent::data` is valid only during its `BlockVisitor` call;
- `RecordView::encoded` and `payload` are valid only during their decode or
  record visitor call.

Copy callback bytes immediately if they must outlive the callback.

### Concurrency model

The supported topology is one writer and multiple readers. `FileStorage`
holds a nonblocking advisory exclusive lock for a writable open. `RingStore`
serializes its own state, while queries work from generation snapshots and can
report a gap if rotation overtakes them.

Use `AsyncWriter` when multiple producer threads need to submit records. Do not
mix direct `RingStore` mutation with an active `AsyncWriter`; keep rotation,
checkpoint, and shutdown under one integration owner. The `Storage` backend
must support concurrent positional reads and one positional writer.

`AsyncWriter` holds a reference, not ownership. The `RingStore` it wraps must
outlive it; destroy or `stop()` the wrapper before the store.

#### Reader freshness

A `RingStore` builds its partition catalog during `open()` and never refreshes
it. Consequences differ by topology:

- readers sharing the writer's `RingStore` observe every published block,
  because the writer updates that one catalog;
- a separate read-only `RingStore` over the same media — in this process or
  another — observes exactly the volume as of its own `open()`. Blocks
  published afterwards are invisible to it for its whole lifetime.

An external process that follows a live volume must therefore reopen to advance
its view, not merely repeat the query. Reopening repeats recovery, including a
bounded block scan of any partition without a valid footer, so poll intervals
should be chosen against measured reopen cost on the target media rather than
assumed to be free.

#### Blocking and observability

`checkpoint()`, `close()`, and rotation call the backend flush while holding the
store's internal lock. Every other `RingStore` entry point, including
`metrics()`, `writer_status()`, `inspect()`, and `query_blocks()`, waits behind
an in-flight flush. Size a health monitor's own timeouts accordingly: on a
stalling device the observation blocks for as long as the flush does.

## Status API

### `enum class StatusCode`

| Value | Meaning and typical response |
|---|---|
| `ok` | operation completed successfully |
| `invalid_argument` | invalid caller input, incompatible configuration, or wrong operation for the object |
| `out_of_range` | offset, length, or record exceeds a representable/configured bound |
| `io_error` | backend I/O failure or attempted mutation through a read-only object |
| `interrupted` | backend operation was interrupted and could not be transparently completed |
| `no_space` | backend range/capacity failure; normal ring rotation is handled internally |
| `corrupt` | reachable metadata/payload violates the format or integrity contract |
| `unsupported` | format feature, compression codec/version, or profile is unavailable |
| `not_found` | requested object is absent; reserved for APIs that distinguish absence |
| `busy` | nonblocking resource unavailable, including writer lock or full async queue |
| `overwritten` | a reader's snapshotted generation was rotated away |
| `generation_exhausted` | the 64-bit partition generation cannot advance |
| `closed` | mutation/async operation attempted after shutdown or without a running writer |
| `internal_error` | an invariant inside TFDB or a supplied extension was violated |

### `class Status`

```cpp
Status();
Status(StatusCode code, const std::string& message);
static Status Ok();
static Status Error(StatusCode code, const std::string& message);
bool ok() const;
explicit operator bool() const;
StatusCode code() const;
const std::string& message() const;
```

A default-constructed status is successful. `status_code_name(code)` returns a
stable lowercase name for logging. It does not include the detail message.

## Byte views

### `class ByteView`

Construct from `(const void*, size)`, a `std::vector<std::uint8_t>`, or as an
empty view. Accessors are `data()`, `size()`, `empty()`, and `valid()`.

`valid()` is false only for a null pointer with nonzero length. Empty views may
have a null pointer. Ring records themselves must be non-empty.

### `class MutableByteView`

The writable equivalent, constructed from `(void*, size)`. Accessors are
`data()`, `size()`, and `valid()`.

Neither class tracks the lifetime of its source buffer.

## Storage backends

### `struct IoResult`

```cpp
std::size_t transferred;
Status status;
```

A backend may return a successful short transfer. It must report the exact
number of bytes transferred and must not over-report the caller's buffer.

### `class Storage`

```cpp
virtual std::uint64_t size() const = 0;
virtual bool writable() const = 0;
virtual IoResult read_at(std::uint64_t offset, MutableByteView output) = 0;
virtual IoResult write_at(std::uint64_t offset, ByteView input) = 0;
virtual Status flush() = 0;
```

Backend contract:

- size is fixed for the open lifetime;
- reads and writes are positional;
- concurrent reads plus one writer are supported;
- a successful `flush()` makes all preceding successful writes durable under
  the documented backend/device contract;
- zero-progress success for a nonempty transfer is invalid;
- unsupported durability must be an error, not a false success.

The ring's power-loss claim is only as strong as this `flush()` contract.
Backend methods used by `AsyncWriter` must not throw; an exception escaping a
worker-thread extension is outside the `Status` model and may terminate the
process.

### Exact I/O helpers

```cpp
Status read_exact(Storage&, std::uint64_t offset, MutableByteView);
Status write_exact(Storage&, std::uint64_t offset, ByteView);
```

These helpers complete bounded short transfers. They retry an
`interrupted` result only when it transferred zero bytes, reject no-progress
success, and reject backends that over-report progress.

### `class FileStorage`

Linux backend for regular files and block devices.

```cpp
static Status open_existing(const std::string& path, bool writable,
                            std::shared_ptr<FileStorage>* output);
static Status create(const std::string& path, std::uint64_t size,
                     bool overwrite,
                     std::shared_ptr<FileStorage>* output);
```

`open_existing()` opens a regular file or block device and determines its
fixed size. A writable open obtains `flock(LOCK_EX | LOCK_NB)`.

`create()` creates/preallocates a regular file, synchronizes the file and its
parent directory, and returns it writable. With `overwrite == false`, an
existing path is rejected. With `overwrite == true`, an existing file is
resized and may lose previous content; keep that mode out of normal startup.

I/O uses `pread`, `pwrite`, and `fdatasync`. `FileStorage` does not enable
`O_DIRECT` and does not issue flash discard/secure erase.

### `class MemoryStorage`

Deterministic volatile/durable image model for unit, crash, and integration
tests. It is mutex-protected and implements `Storage`.

| API | Test behavior |
|---|---|
| `add_fault(FaultRule)` | inject a non-OK zero-transfer result, or a successful bounded short transfer, at a one-based call |
| `clear_faults()` | remove configured rules |
| `set_hook(StorageHook)` | observe/block before or after individual storage calls |
| `crash_discard_volatile()` | discard every unflushed change |
| `crash_persist_all_dirty()` | model a crash where all dirty writes happened to persist |
| `crash_materialize(fragments)` | persist selected pieces of dirty writes in explicit order, discard the rest |
| `corrupt_durable(offset, mask)` | XOR one byte in the durable image |
| `corrupt_volatile(offset, mask)` | XOR one byte in the current volatile image |
| `counters()` / `events()` | obtain exact operation metrics/history |
| `reset_counters()` | start a new call-number/measurement epoch without clearing bytes or fault rules |
| `durable_image()` / `volatile_image()` | copy an image for assertions or reopen tests |

`FaultRule::call` is one-based per `StorageOperation`. `WriteFragment` selects a
successful write call, offset within that write, and length.

`StorageHook` receives `(operation, HookPoint, call, offset, requested_size)`.
Release any application lock needed by the hook before allowing another thread
to enter the backend.

### `class CountingStorage`

A mutex-protected decorator over another `Storage`. It forwards all behavior
and exposes `StorageCounters` through `counters()` and `reset_counters()`.
The delegate must be non-null and must outlive in-flight calls through its
shared ownership.

`StorageCounters` contains call and byte counts for reads/writes plus flush
calls. Counters measure reported transferred bytes, not physical NAND writes.

## Compression API

### `enum class CompressionId`

| ID | Value | Availability |
|---|---:|---|
| `none` | 0 | built in |
| `packbits` | 1 | built in, dependency-free |
| `lz4_block` | 2 | reserved; not implemented by the core |

### `class CompressionCodec`

```cpp
virtual CompressionId id() const = 0;
virtual std::uint16_t version() const = 0;
virtual std::size_t max_compressed_size(std::size_t input_size) const = 0;
virtual Status compress(ByteView input,
                        std::vector<std::uint8_t>* output) const = 0;
virtual Status decompress(ByteView input, std::size_t expected_size,
                          std::vector<std::uint8_t>* output) const = 0;
```

A custom codec instance must be safe for the store's reader/writer topology.
`compress()` must stay within its declared maximum. `decompress()` must produce
exactly `expected_size`; TFDB rejects a mismatch.
Codec methods must not throw when they may run on the async worker; report
extension failures with `Status`. The input view may refer to the current
contents of `*output`; consume it before changing output or build into a
temporary vector and swap. Both built-in codecs support aliased input/output.

Factory functions:

```cpp
std::shared_ptr<const CompressionCodec> no_compression_codec();
std::shared_ptr<const CompressionCodec> packbits_codec();
std::shared_ptr<const CompressionCodec> built_in_codec(CompressionId id);
```

When compression is configured, TFDB stores a block compressed only if the
result is smaller; otherwise that block uses `CompressionId::none`.

## Volume and partition configuration

### `struct VolumeOptions`

Used only by `RingStore::format()`.

| Field | Default | Contract |
|---|---:|---|
| `partition_size` | 64 MiB | fixed partition span; quantum multiple |
| `index_region_size` | 1 MiB | reserved time-index bytes per partition; quantum multiple |
| `max_block_payload` | 32 KiB | maximum uncompressed block payload and maximum single record size |
| `persistence_quantum` | 4096 | power of two, at most 4096; logical separation between block starts |
| `volume_id_high/low` | 0/0 | zero requests a random 128-bit incarnation ID |
| `allow_explicit_volume_id_for_testing` | false | must be true to use deterministic nonzero IDs; never enable in production |
| `created_time_ns` | 0 | informational volume creation time supplied by the application |

`persistence_quantum` is a layout/durability isolation unit, not an
`O_DIRECT` alignment promise.

### `struct PartitionOptions`

Persisted in every partition header.

| Field | Default | Contract |
|---|---:|---|
| `compression` | `none` | configured codec; individual blocks may fall back to none |
| `compression_version` | 1 | must match a registered codec |
| `record_format_id` | 0 | zero means opaque/native blocks without record-profile declaration |
| `record_format_version` | 0 | both profile ID and version must be zero or both nonzero |
| `time_domain_id` | 1 | nonzero stable project time-domain identifier |
| `allowed_backward_skew_ns` | -1 | negative disables backward anomaly detection |
| `allowed_forward_step_ns` | -1 | negative disables forward anomaly detection |

Configuration changes apply at partition boundaries and require no migration
of older partitions.

### `struct AppendContract`

```cpp
std::uint64_t record_format_id;
std::uint16_t record_format_version;
std::uint64_t time_domain_id;
```

All three fields must be nonzero for checked append/submit. The contract is
compared with the active partition after any automatic rotation and before the
record is copied into the builder.

### `struct OpenOptions`

| Field | Default | Meaning |
|---|---:|---|
| `writable` | false | recover as a writer and create/continue the active partition |
| `verify_payloads_on_open` | false | strictly scan/CRC-check all reachable blocks and exercise compression decode/raw-size validation |
| `next_partition` | defaults above | contract for a newly created/next automatic partition |
| `compression_codecs` | empty | additional codec implementations needed by retained media or next partition |
| `writer_id_high/low` | 0/0 | zero requests a random writer-incarnation ID |
| `allow_explicit_writer_id_for_testing` | false | deterministic writer IDs are tests/golden vectors only |

A read-only open does not create a partition. A writable open refuses ambiguous
damaged partition headers because it cannot safely choose what to overwrite.
Strict open does not invoke `RecordProfile` and therefore does not validate
native record framing or application payload schemas.

## Ring store API

### Formatting and open

```cpp
static Status RingStore::format(Storage&, const VolumeOptions&);
static Status RingStore::open(std::shared_ptr<Storage>, const OpenOptions&,
                              std::unique_ptr<RingStore>* output);
```

`format()` writes and flushes a fresh duplicated volume header. It requires a
writable backend and valid geometry. Existing partition bytes may remain
physically present but belong to a different volume incarnation and are not
reachable.

`open()` validates the volume, discovers generations, uses sealed indexes or
bounded block scans, and reconstructs the active partition. The store retains
the backend `shared_ptr` for its lifetime. The catalog it builds is fixed for
the store's lifetime; see [reader freshness](#reader-freshness).

Both `format()` and a writable `open()` draw a fresh 128-bit incarnation ID
from Linux `getrandom()`, which **blocks until the kernel CSPRNG is seeded**.
On a headless target with no hardware entropy source that can take seconds or
longer immediately after boot, so a service that opens its store early in
startup should either accept that wait or defer the writable open until the
system reports the pool as initialized. Read-only opens draw no randomness and
are unaffected.

### Append and durability

```cpp
Status append(ByteView encoded_record, std::int64_t index_time_ns,
              std::uint32_t record_flags = 0);
Status append_checked(ByteView encoded_record,
                      std::int64_t index_time_ns,
                      const AppendContract& expected,
                      std::uint32_t record_flags = 0);
Status publish();
Status checkpoint();
Status emergency_checkpoint();
Status close();
```

`append*()` accepts exactly one whole nonempty record, copies it, and preserves
physical call order. A record larger than `max_block_payload` returns
`out_of_range`. If it does not fit the current builder, the previous builder is
published first. Full partitions rotate automatically.

`append_checked()` additionally prevents records from crossing into a
partition with a different record profile/version/time domain.

Application record flags occupy bits 16..31 of the 32-bit argument. Bit 0 is
reserved as `kRecordFlagUnsynchronizedTime` and is translated to the block's
`kBlockFlagUnsynchronizedTime`. Other low bits are reserved and ignored.

`publish()` emits a complete CRC-protected block but does not call `flush()`.
`checkpoint()` publishes the current tail and makes preceding successful
writes durable. `emergency_checkpoint()` is an alias. `close()` performs the
graceful final checkpoint and then causes mutating calls to return `closed`;
repeated close succeeds.

The destructor does not checkpoint.

### Rotation and future configuration

```cpp
Status rotate(const PartitionOptions& next_options);
Status set_next_partition_options(const PartitionOptions& options);
```

`rotate()` publishes the builder, makes existing active data durable, writes
and flushes the partition index/footer, then creates and flushes the next
generation. It can overwrite the oldest slot by design.

`set_next_partition_options()` changes only future automatic rotation. It
validates codec/profile/time-domain consistency immediately.

### Block queries

```cpp
Status query_blocks(const TimeRange&, const QueryOptions&,
                    const BlockVisitor&) const;
Status scan_blocks(const QueryOptions&, const BlockVisitor&) const;
```

`query_blocks()` uses per-block min/max time and filters by a nonzero time
domain. A block is a candidate when its inclusive min/max envelope overlaps
the requested half-open range. Individual records are not inspected.

`scan_blocks()` visits all currently reachable blocks across time domains in
generation/block sequence order.

Both APIs take a snapshot, always verify block CRCs, decompress payloads, and
emit `BlockEvent`:

```cpp
enum class BlockEventKind { data, overwritten_gap, corrupt_gap };
```

| Kind | `data` | `detail` | Meaning |
|---|---|---|---|
| `data` | borrowed decompressed block | OK | one valid reachable block |
| `overwritten_gap` | empty | `overwritten` | rotation replaced a snapshotted generation while reading |
| `corrupt_gap` | empty | `corrupt` or related detail | reachable partition/block cannot be validated |

`QueryOptions::continue_on_gap` defaults to true. When false, the gap is first
delivered to the visitor and then returned as the query status. Returning false
from the visitor is a successful early stop.

`QueryOptions::verify_payload_crc` is ignored; live-ring CRC validation is
mandatory.

### Inspection

```cpp
Status inspect(VolumeInfo* output) const;
Status active_partition_info(PartitionInfo* output) const;
StoreMetrics metrics() const;
Status writer_status() const;
bool writable() const;
```

`inspect()` returns volume geometry/ID plus known partitions. `partitions` are
reported in generation order. `active_partition_info()` returns `closed` when
there is no active partition, including a read-only empty volume.

`PartitionInfo` contains slot, generation, sealed state, block count, data
bytes, time bounds, aggregate flags, and its persisted `PartitionOptions`.
Zero time bounds on an empty partition do not establish that its time domain
contains timestamp zero.

`writable()` reports the immutable capability selected at open. It remains true
after close or a writer fault. `writer_status()` returns OK only while mutations
can currently be accepted; otherwise it returns read-only `invalid_argument`,
`closed`, or the original stored fault. Use it before attaching a new async
wrapper and in writer health checks.

### Block flags

| Constant | Meaning |
|---|---|
| `kBlockFlagTimeAnomaly` | configured adjacent timestamp threshold was exceeded |
| `kBlockFlagUnsynchronizedTime` | at least one record was marked unsynchronized |
| `kBlockFlagApplicationBase` | first bit available for project block flags (`1u << 16`) |
| `kRecordFlagUnsynchronizedTime` | record input flag translated to the block unsynchronized flag |

Anomalies are diagnostic: the record is retained.

### `BlockMetadata`

For each event, metadata identifies:

- `partition_slot`, `partition_generation`, and `block_sequence`;
- `record_count`, min/max indexed time, and flags;
- actual block compression, stored/raw sizes, and physical offset;
- partition record format ID/version and time-domain ID.

The physical offset is an inspection/export aid. It is not a stable record ID
because ring rotation overwrites slots.

### `StoreMetrics`

| Field group | Fields |
|---|---|
| Accepted/published | `accepted_records`, `accepted_payload_bytes`, `published_blocks`, `stored_block_bytes`, `raw_block_bytes` |
| Durability/rotation | `checkpoints`, `rotations`, `durable_records`, `durable_blocks` |
| Data quality | `time_anomalies`, `overwritten_gaps`, `corrupt_gaps` |
| Backend sync | `sync_errors`, `sync_calls`, `last_sync_duration_ns`, `max_sync_duration_ns` |
| Exposure | `oldest_undurable_age_ns`, `max_accepted_to_durable_ns` |

Metrics are process-lifetime observations for this open store, not persistent
counters. Sample them for health monitoring; do not reconstruct retention from
them.

## Record profiles

### `struct RecordView`

```cpp
ByteView encoded;
ByteView payload;
std::int64_t index_time_ns;
std::uint64_t selector;
std::uint16_t flags;
```

Both views are callback-scoped. `encoded` contains the native complete record;
`payload` may be a subview.

### `class RecordProfile`

```cpp
virtual std::uint64_t id() const = 0;
virtual std::uint16_t version() const = 0;
virtual Status decode_block(ByteView block,
                            const RecordDecodeVisitor& visitor) const = 0;
```

A decoder validates concatenated records and invokes the visitor in block
order. Return `corrupt` for malformed records. Respect visitor false as normal
early termination. IDs and versions must match the partition metadata.

### `struct RecordQuery`

Contains `TimeRange time`, a vector of `selectors`, and block-level
`QueryOptions`. An empty selector vector means every selector. Duplicate input
selectors do not duplicate output.

### `query_records()`

```cpp
Status query_records(const RingStore&, const RecordQuery&,
                     const RecordProfile&, const RecordVisitor&);
```

This first selects candidate blocks by time domain/envelope, requires their
profile ID/version to match, decodes records, then applies exact half-open time
and selector filtering. It also verifies that the decoder emitted exactly the
block's declared record count.

Gap events are forwarded as `RecordEvent` with an empty/default `record` and
the original block metadata/detail.

### `class FramedRecordV1`

Built-in record profile ID:

```cpp
constexpr std::uint64_t kFramedRecordV1ProfileId;
```

API:

```cpp
static Status encode(std::int64_t index_time_ns,
                     std::uint64_t selector,
                     std::uint16_t flags,
                     ByteView payload,
                     std::vector<std::uint8_t>* output);
Status decode_block(ByteView, const RecordDecodeVisitor&) const override;
```

The 32-byte envelope is little-endian and protects header plus payload with
CRC32C. `encode()` replaces the output vector. The maximum payload is bounded
by a 32-bit frame size and, in practice, by the volume's block limit.
The payload view may refer to the current output vector; encoding remains
well-defined because the new frame is built separately and swapped into place.

Convenience append:

```cpp
Status append_framed_record(RingStore&, std::int64_t index_time_ns,
                            std::uint64_t selector, std::uint16_t flags,
                            ByteView payload);
```

It encodes, checks the active FramedRecordV1 partition, preserves the active
time domain in an `AppendContract`, and maps the unsynchronized-time flag.

## Asynchronous writer

### `struct AsyncWriterOptions`

| Field | Default | Contract |
|---|---:|---|
| `queue_capacity_records` | 4096 | nonzero accepted-item bound |
| `queue_capacity_bytes` | 4 MiB | nonzero copied-byte bound |
| `checkpoint_interval` | 1000 ms | positive periodic durability interval |
| `monotonic_clock_ns` | empty | optional nonblocking/nonthrowing monotonic clock for deterministic tests |

The custom clock must be advanced externally. After advancing it, release any
clock mutex and call `notify_clock_advanced()` so the worker re-evaluates its
deadline. Production normally uses the default steady clock.

### Lifecycle

```cpp
AsyncWriter(RingStore& store, const AsyncWriterOptions& options);
Status start();
Status submit(ByteView, std::int64_t index_time_ns,
              std::uint32_t record_flags = 0);
Status submit_checked(ByteView, std::int64_t index_time_ns,
                      const AppendContract&,
                      std::uint32_t record_flags = 0);
Status checkpoint();
Status stop();
void notify_clock_advanced();
```

`start()` validates options and starts one worker. Repeated start while running
returns `busy`. A wrapper whose worker already failed cannot be restarted; open
a fresh store and construct a new wrapper. Start also checks
`RingStore::writer_status()`, so a closed or faulted store fails synchronously.

`submit*()` is thread-safe and nonblocking with respect to storage I/O. It
copies accepted bytes. A full record or byte bound returns `busy` without
accepting the item. Submit before start, during stop, or after stop returns
`closed` after a clean lifecycle. If the worker failed, its original status has
priority and is returned by later submit/checkpoint/start calls.

`checkpoint()` is a synchronous barrier: after success, every submit accepted
before the call is processed and durable. It does not promise that concurrently
accepted later items are included.

`stop()` stops acceptance, drains the queue, checkpoints, joins, and returns
the background status. The destructor calls `stop()` but discards the result;
production shutdown must call and check it explicitly.

### State and metrics

```cpp
std::size_t queued_records() const;
std::size_t queued_bytes() const;
Status background_status() const;
AsyncWriterMetrics metrics() const;
```

`AsyncWriterMetrics` fields:

| Group | Fields |
|---|---|
| Flow | `submitted_records`, `processed_records`, `durable_sequence` |
| Durability/errors | `checkpoint_calls`, `background_errors` |
| Backpressure/high water | `backpressure_events`, `maximum_queued_records`, `maximum_queued_bytes` |
| Latency | `maximum_queue_residence_ns`, `maximum_accepted_to_durable_ns`, `oldest_undurable_age_ns` |

`durable_sequence` is the highest accepted wrapper sequence covered by a
successful checkpoint, not a persistent record sequence. Queue residence
excludes RingStore/backend work; accepted-to-durable includes it.

On a background failure, the writer stores the first error, clears queued
items, increments `background_errors`, and wakes barriers. Callers must surface
the error and recreate the storage stack after recovery.

## Compatibility and extension rules

- Treat the on-media major version, compression ID/version, record profile
  ID/version, time-domain ID, selector layout, and application flag meanings
  as separate compatibility axes.
- Add incompatible record encodings at a partition boundary; do not reinterpret
  historical bytes under a new profile version.
- Register compression codecs for every retained historical partition before
  enabling a new writer configuration. Record profiles have no store registry:
  `query_records()` handles one profile pair per call and returns `unsupported`
  on a candidate partition with another pair. For a range spanning profile
  versions, use `query_blocks()` and application-level dispatch through
  `BlockMetadata`, then perform exact record filtering in that layer.
- Unknown required on-media features fail open as `unsupported`.
- Keep explicit volume/writer IDs restricted to deterministic tests.
- Do not persist C++ object layouts directly; encode project records with
  fixed widths/endianness and explicit bounds.

## API integration review checklist

- Only public headers under `include/tfdb` are included.
- All status returns are checked and status codes, not message strings, drive
  behavior.
- The backend lifetime exceeds the `RingStore`; the store lifetime exceeds
  `AsyncWriter`.
- Format is reachable only from explicit provisioning/reinitialization.
- The record/time-domain contract is chosen before writable open.
- Checked append/submit protects partition rotations.
- `publish()` is never described or used as a durability boundary.
- Async `busy`, background failure, checkpoint barrier, and orderly stop have
  explicit policies.
- Query callbacks copy retained data and handle both gap kinds.
- Physical query order is not confused with timestamp order.
- Metrics and target-hardware qualification demonstrate the configured
  power-loss and endurance requirements.

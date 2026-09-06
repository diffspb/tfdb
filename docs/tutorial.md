# TFDB integration tutorial

TFDB means **Telemetry Flash DB**. It is an embedded C++14 library for a
single telemetry or log writer, concurrent readers, bounded recovery, and
fixed-size flash-backed retention. This tutorial starts with a runnable store
and then turns it into a production integration.

The canonical API reference is [`api.md`](api.md). Storage sizing and the
durability model are developed in [`sizing.md`](sizing.md) and
[`architecture.md`](architecture.md).

## 1. Choose the integration shape

Make four decisions before writing adapters:

| Decision | Simple default | Choose the other path when |
|---|---|---|
| Record format | `FramedRecordV1` | project records already have unambiguous self-framing, timestamp, and selector |
| Writer | synchronous `RingStore` | producers must not perform storage I/O and bounded asynchronous backpressure is acceptable |
| Backend | `FileStorage` | the application already has a storage abstraction with equivalent positional-I/O and flush guarantees |
| Reader | `query_records()` | the consumer wants whole physical blocks and will decode or sort them elsewhere |

`FramedRecordV1` is an optional 32-byte envelope. It supplies a length,
application-provided index timestamp, 64-bit selector, flags, payload length,
and CRC. Native self-framing telemetry can be appended directly and decoded by
a project-specific `RecordProfile`, avoiding double framing.

For a first integration, use `FramedRecordV1`, the synchronous writer, and a
regular file. Change one layer at a time after the lifecycle works.

## 2. Build and link

Build the dependency-free static library on Linux:

```sh
make
```

Compile application code as C++14, add `include/` to the include path, link
`build/libtfdb.a`, and enable pthreads:

```sh
c++ -std=c++14 -pthread -I/path/to/tfdb/include app.cpp \
    /path/to/tfdb/build/libtfdb.a -o app
```

The repository contains two executable examples:

```sh
make -C examples check
```

This builds and runs:

- [`examples/quickstart.cpp`](../examples/quickstart.cpp): format, synchronous
  append, checkpoint, selector/time query, and graceful close;
- [`examples/async_writer.cpp`](../examples/async_writer.cpp): bounded queue,
  checked submit, durability barrier, metrics, and shutdown.

Both examples require a path that does not already exist. They never overwrite
an existing store.

CMake consumers can build TFDB directly with `add_subdirectory()` and link
`TFDB::tfdb`, or use the installed `TFDBConfig.cmake`:

```cmake
find_package(TFDB 1 CONFIG REQUIRED)
target_link_libraries(my_service PRIVATE TFDB::tfdb)
```

Meson consumers can use TFDB as a subproject and request
`dependency('tfdb')`; installation also emits `tfdb.pc`:

```meson
tfdb_dep = dependency('tfdb')
executable('my_service', 'main.cpp', dependencies: tfdb_dep)
```

Build-system options are `TFDB_BUILD_TOOLS`, `TFDB_BUILD_EXAMPLES`, and the
standard CMake `BUILD_TESTING`; Meson uses `-Dtools=`, `-Dexamples=`, and
`-Dtests=`. None changes the media format.

The shorter fragments below focus on one API decision at a time: `report()`
and `check()` stand for application helpers that log
`status_code_name(status.code())` plus `status.message()` and propagate every
failure. The linked example translation units contain the complete includes,
variables, and checked control flow.

## 3. Provision a store once

Formatting is a provisioning or explicit reinitialization operation. Do not
put it unconditionally in normal application startup.

```cpp
std::shared_ptr<tfdb::FileStorage> storage;
tfdb::Status status = tfdb::FileStorage::create(
    path, fixed_size, false, &storage);  // false: refuse to overwrite
if (!status.ok()) return report(status);

tfdb::VolumeOptions volume;
volume.partition_size = 64ull * 1024ull * 1024ull;
volume.index_region_size = 1024u * 1024u;
volume.max_block_payload = 32u * 1024u;
volume.persistence_quantum = 4096u;
status = tfdb::RingStore::format(*storage, volume);
if (!status.ok()) return report(status);
```

For an existing block device, use
`FileStorage::open_existing(device_path, true, &storage)` and call `format()`
only after the application has separately established that destructive
initialization is intended. Before that call, deployment tooling must verify
the exact device identity and expected size, confirm that it is unmounted and
not in use, and establish exclusive ownership by TFDB. The advisory writer
lock protects only cooperating processes. `FileStorage` accepts only regular
files and block devices.

Geometry is immutable for the volume:

- storage must contain at least two partitions after the volume prefix;
- `persistence_quantum` must be a power of two and no larger than 4096;
- partition and index sizes must be quantum multiples;
- the data region must fit one worst-case block;
- the index reserve must fit the number of expected blocks.

Do not copy tutorial geometry into production unchanged. Calculate retention,
write amplification, index reserve, and eight-year endurance with
[`sizing.md`](sizing.md), then qualify them on the target eMMC/NAND controller.

## 4. Open an existing store

Normal startup opens the already provisioned file or block device and lets
`RingStore::open()` recover the current ring:

```cpp
std::shared_ptr<tfdb::FileStorage> storage;
tfdb::Status status =
    tfdb::FileStorage::open_existing(path, true, &storage);
if (!status.ok()) return report(status);

tfdb::OpenOptions options;
options.writable = true;
options.next_partition.record_format_id = tfdb::kFramedRecordV1ProfileId;
options.next_partition.record_format_version = 1;
options.next_partition.time_domain_id = 1;
options.next_partition.compression = tfdb::CompressionId::none;

std::unique_ptr<tfdb::RingStore> store;
status = tfdb::RingStore::open(storage, options, &store);
if (!status.ok()) return report(status);
```

`next_partition` is the contract used if open must create a partition and for
later automatic rotations. Existing partitions retain the options recorded in
their own headers. This is what allows configuration and software updates
without migrating old data.

Only one writable `FileStorage` can hold the advisory exclusive lock. Any
number of readers may open the media read-only, but applications should prefer
queries through the writer-owned `RingStore` when possible so recovery state
and codecs are consistent.

Use `verify_payloads_on_open = true` for strict startup validation. It reads
and CRC-checks every reachable block and exercises its compression decoder and
declared raw size, so it increases startup I/O. It does not validate native
record framing or application schemas; record queries do that through a
`RecordProfile`. The default performs bounded structural recovery and defers
full block payload reads to queries.

## 5. Map application telemetry to records

TFDB deliberately does not prescribe the payload schema. Define these project
contracts outside the library:

1. a stable `record_format_id` and version;
2. a stable `time_domain_id` for the application-provided index time;
3. a 64-bit selector mapping if record-level filtering is required;
4. payload serialization and schema/version lookup;
5. the policy for unsynchronized and anomalous time.

A useful selector packs stable fields such as source address, Linux service,
and message identifier. The exact bit layout belongs to the project protocol,
not to TFDB. Never derive it from a transient process ID or startup order.

Use arrival time at the storage service as `index_time_ns` when source clocks
can start in 1970 or otherwise become unreliable. Preserve the source-provided
timestamp inside the payload for later diagnosis or repair. This keeps the
physical/time index useful even when one producer clock is wrong.
For systems where the storage host clock can also start unsynchronized, retain
arrival monotonic time and boot/session identity as well; see the non-mutating
proposal in [`time-reconstruction.md`](time-reconstruction.md).

For `FramedRecordV1`:

```cpp
tfdb::Status status = tfdb::append_framed_record(
    *store,
    arrival_time_ns,
    selector,
    source_time_is_unsynchronized ? tfdb::kRecordFlagUnsynchronizedTime : 0,
    tfdb::ByteView(payload));
```

`append_framed_record()` verifies that the active partition still has the
FramedRecordV1 profile and expected time domain. This matters when a record
causes automatic rotation.

The optional `allowed_backward_skew_ns` and `allowed_forward_step_ns`
thresholds diagnose suspicious adjacent index timestamps. A violation sets
`kBlockFlagTimeAnomaly`; the record is still retained. Negative thresholds
disable the corresponding test. Record order is always arrival/append order,
not timestamp order.

## 6. Understand publish and durability

The synchronous lifecycle has three distinct states:

| Call | Effect | Durable after successful return? |
|---|---|---:|
| `append()` / `append_checked()` | copies one whole record into the RAM block builder; may publish a previous full block | no |
| `publish()` | emits the current builder as a complete block | no |
| `checkpoint()` | publishes the tail and calls backend `flush()` | yes, according to the backend/device contract |
| `close()` | graceful final checkpoint and rejects later mutations | yes |

The destructor intentionally does not checkpoint. Destruction is not an error
reporting channel, and pretending it is a durability boundary would hide
failures. Always call and check `close()` on graceful shutdown.

To bound power-loss exposure to `X` seconds, the application must successfully
checkpoint at least every `X` seconds under the qualified worst-case workload.
Observe `StoreMetrics::oldest_undurable_age_ns`,
`max_accepted_to_durable_ns`, `sync_errors`, and `max_sync_duration_ns`. The
timer alone is not evidence; queue residence and device flush latency count.

After any write or flush error, treat the writer as faulted. Stop accepting
upstream data according to the system policy, preserve the original `Status`,
and reopen/recover a new `RingStore` before resuming.

## 7. Use the bounded asynchronous writer

`AsyncWriter` owns a worker thread but not the `RingStore`. The store and its
backend must outlive the wrapper.

```cpp
tfdb::AsyncWriterOptions options;
options.queue_capacity_records = 4096;
options.queue_capacity_bytes = 4u * 1024u * 1024u;
options.checkpoint_interval = std::chrono::milliseconds(1000);

tfdb::AsyncWriter writer(*store, options);
check(writer.start());

tfdb::AppendContract contract;
contract.record_format_id = my_profile_id;
contract.record_format_version = my_profile_version;
contract.time_domain_id = my_time_domain_id;

check(writer.submit_checked(tfdb::ByteView(encoded_record), index_time_ns,
                            contract, flags));
check(writer.checkpoint());  // barrier for every prior successful submit
check(writer.stop());        // drains and checkpoints before joining
check(store->close());
```

`submit()` and `submit_checked()` copy the input before returning. Calls may
come from multiple producer threads. If either queue bound would be exceeded,
the call returns `StatusCode::busy`; it does not silently drop or block. Decide
outside TFDB whether to retry, shed lower-priority data, or apply upstream
backpressure.

Poll `background_status()` and metrics in service health reporting. A worker
error clears the remaining queue, wakes checkpoint waiters, and requires a new
store/writer instance. The wrapper retains the original background status and
`stop()` returns it; the destructor invokes `stop()` but discards that result.

## 8. Query records

Record queries apply exact half-open timestamp and selector filtering after a
coarse block-index selection:

```cpp
tfdb::RecordQuery query;
query.time.begin_ns = begin_ns;
query.time.end_ns = end_ns;  // [begin_ns, end_ns)
query.time.time_domain_id = 1;
query.selectors = requested_selectors;  // empty means every selector

tfdb::FramedRecordV1 profile;
tfdb::Status status = tfdb::query_records(
    *store, query, profile,
    [&](const tfdb::RecordEvent& event) {
      if (event.kind != tfdb::BlockEventKind::data) {
        report_gap(event.kind, event.block, event.detail);
        return true;
      }
      consume_now(event.record);  // views are borrowed for this callback only
      return true;
    });
```

Important query contracts:

- results are in physical ring order; timestamps may be mixed and are not
  sorted;
- `RecordView::encoded`, `RecordView::payload`, and `BlockEvent::data` are valid
  only during their callback;
- returning `false` stops normally and the query returns `Status::Ok()` unless
  decoding already failed;
- readers can lose a race with rotation; `overwritten_gap` is expected when a
  reader cannot keep up;
- damaged reachable media is reported as `corrupt_gap` when
  `continue_on_gap` is true;
- set `continue_on_gap = false` when the first gap must terminate the query;
- CRC verification is mandatory; `verify_payload_crc` remains only for source
  compatibility and is ignored.

If a consumer wants candidate blocks, decompressed bytes, and physical
metadata, call `query_blocks()`. Use `scan_blocks()` to walk every reachable
block across all time domains. Block-level time selection is necessarily
coarse: a block is returned if its `[min_time_ns, max_time_ns]` overlaps the
query, even when no individual record matches.

## 9. Use a native record format

If project records are already concatenable, append their complete encoded
bytes directly. Every appended item must be whole, non-empty, and no larger
than `max_block_payload`.

Implement `RecordProfile` to decode one decompressed block:

```cpp
class ProjectProfile final : public tfdb::RecordProfile {
 public:
  std::uint64_t id() const override { return kProjectProfileId; }
  std::uint16_t version() const override { return 3; }

  tfdb::Status decode_block(
      tfdb::ByteView block,
      const tfdb::RecordDecodeVisitor& visitor) const override {
    // Validate each frame before exposing it. Return corrupt for malformed
    // lengths/checksums, and stop successfully when visitor(record) is false.
    // RecordView fields must point into storage valid for this call only.
  }
};
```

Open with that ID/version, construct a nonzero `AppendContract`, and prefer
`append_checked()` or `submit_checked()`. The decoder must emit exactly the
block header's record count; `query_records()` rejects a mismatch as
corruption.

Keep record-profile IDs globally stable across projects that exchange media.
A new incompatible record encoding requires a new version and starts at a
partition boundary. Register every historical compression codec in
`OpenOptions` before open. Record profiles have no built-in registry:
`query_records()` accepts one profile and returns `unsupported` if a candidate
partition uses another ID/version. To read a time range spanning profile
versions, use `query_blocks()` and dispatch/decode each block in the application
from `BlockMetadata::record_format_id` and `record_format_version`, including
exact record filtering there.

## 10. Change configuration at a partition boundary

Compression, record profile, time domain, and anomaly thresholds are recorded
per partition.

- `set_next_partition_options()` changes what the next automatic rotation will
  use; it does not alter the active partition.
- `rotate()` publishes and seals the current partition, durably writes its
  index/footer, then starts the requested configuration immediately.
- an automatic rotation uses the most recently configured next options.

An asynchronous producer can have queued records while configuration changes.
Use `submit_checked()` with the contract attached to each queued record; a
stale record then fails explicitly instead of being written under the wrong
profile or time domain.

The built-in dependency-free codecs are `none` and PackBits. Compression is
selected per block only when the compressed representation is smaller. The
`lz4_block` identifier is reserved but has no built-in implementation; supply
a matching `CompressionCodec` in `OpenOptions::compression_codecs` before
opening media that uses an external codec.

## 11. Store application logs

Treat logs as structured records rather than an unbounded text stream. A
selector can encode service, severity, or schema; the payload can contain the
message and structured attributes. Decide whether logs and telemetry share a
volume based on retention and failure-domain requirements.

Do not place secrets in logs merely because the store is local. TFDB v1 does
not provide encryption, authentication, redaction, or secure erasure. The full
log design checklist is in [`logs.md`](logs.md).

## 12. Implement a custom backend

Derive from `Storage` only when `FileStorage` is not suitable. The backend must:

- provide a fixed nonzero `size()` for the open lifetime;
- support concurrent positional readers and one positional writer;
- report partial progress accurately through `IoResult`;
- make no-progress success impossible;
- make every preceding successful write durable when `flush()` succeeds;
- return a stable error when durability cannot be established.

TFDB's `read_exact()` and `write_exact()` handle bounded short transfers and
retry zero-progress `interrupted` calls. They cannot repair a backend that lies
about durability or transfer length.

Use `MemoryStorage` and `CountingStorage` in integration tests to inject
failures, model crash persistence, and assert exact I/O. They are support
backends, not replacements for target-device power-cut qualification.

## 13. Operational checks

After building the supplied tools:

```sh
make tools
build/tools/tfdb_inspect STORE
build/tools/tfdb_verify STORE
build/tools/tfdb_dump STORE --framed-v1 --from BEGIN_NS --to END_NS \
    --selector SELECTOR
```

`tfdb_verify` reports gaps and validates reachable blocks. It does not prove
the underlying flash controller's power-loss behavior. Run the qualification
matrix in [`testing.md`](testing.md) on every target storage/controller/firmware
combination.

## Production integration checklist

- Provisioning and normal startup are separate code paths.
- Every `Status`, including `checkpoint()`, `stop()`, and `close()`, is checked.
- One writer owns the writable backend; reader behavior during overwrite is
  explicitly handled.
- The index timestamp comes from a defined application clock; the source clock
  remains in the payload when needed.
- Record format, selector layout, time-domain IDs, and flag meanings are
  versioned project contracts.
- Checked append/submit is used for nonzero record profiles.
- Queue bounds and the `busy` policy are deliberate and load-tested.
- The checkpoint interval plus worst queue/flush latency meets the power-loss
  budget.
- Geometry and compression are validated against retention, RAM, and flash
  endurance requirements.
- Shutdown drains the async writer before closing the store.
- Gaps, time anomalies, sync errors, backpressure, and durability age feed
  operational telemetry.
- Physical power-cut and long-duration target-media tests pass before release.

## Common mistakes

| Mistake | Consequence | Correct action |
|---|---|---|
| formatting on every boot | destroys the retained ring | format only during explicit provisioning/reinitialization |
| relying on `RingStore` destruction | RAM tail may be lost and errors disappear | explicitly check `close()` |
| treating `publish()` as durable | a power cut may lose published blocks | use and check `checkpoint()` |
| using source-device time as the only index | 1970-era or bad clock values make extraction unreliable | index by storage-service arrival time; retain source time in payload |
| assuming query timestamps are sorted | mixed sources produce out-of-order output | consume physical order or sort after extraction |
| retaining callback views | use-after-lifetime bugs | copy bytes inside the callback |
| retrying `busy` in a tight loop | CPU burn and unbounded latency | apply bounded retry/backpressure/drop policy outside TFDB |
| changing profile without rotation discipline | queued records can enter the wrong contract | use checked submit and rotate at a controlled boundary |
| ignoring gap events | incomplete exports look complete | surface and count overwritten/corrupt gaps |
| using deterministic IDs in production | weakens incarnation identity | leave explicit test-ID fields zero |

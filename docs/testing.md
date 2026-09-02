# Verification and acceptance strategy

Testing is part of the storage contract. The same backend abstraction used by
the POSIX implementation is implemented by a deterministic memory model with a
volatile image, durable image, operation counters, and programmable failures.
Current reproducible results are recorded in [`evidence.md`](evidence.md); this
document also defines qualification work that must be repeated for each target
project and hardware stack.

## 1. Test layers

### Unit tests

- explicit-endian integer codecs and overflow/bounds rejection;
- CRC known vectors and corruption detection;
- compression round trips, incompressible fallback, malformed input, and
  maximum expansion;
- volume/partition/block/index/footer serialization golden bytes;
- time-range intersection with ordered, disordered, repeated, negative, and
  extreme timestamps;
- whole-record block packing and oversize rejection;
- generation ordering and rotation selection;
- status/error propagation and diagnostic counters.

### Model and state-machine tests

A small reference model stores accepted records by generation and physical
order. The current deterministic crash model covers append, checkpoint, crash,
and reopen; focused tests cover query, rotation, corruption, and
partition-local configuration changes. Extending the randomized command model
to combine all of those operations remains a pre-freeze qualification item.

Properties include:

- every returned record was appended and not silently altered;
- physical order never reverses;
- normal time queries have no false-negative block for valid min/max metadata;
- every record in the durable retained-window baseline remains present exactly
  and in physical order unless a later successfully flushed rotation evicted
  its generation; a modeled crash may add only specifically materialized,
  whole CRC-valid frames after that baseline;
- reuse never exposes blocks from a stale generation as current;
- bounded structures reject overflow rather than wrapping arithmetic.

### Crash/fault injection

The memory backend records exact read/write/flush calls and bytes. A flush copies
the volatile image to the durable image. Crash discards volatile state. Failure
plans can:

- return `EINTR`, a zero-progress success, or a successful short read/write;
- return a selected hard error with no transferred bytes;
- fail a selected write or flush call;
- preserve arbitrary prefixes/subsets of writes since the last flush;
- materialize selected writes in controller-reordered order;
- flip bits in header, payload, index, footer, or stale region;
- simulate crash after every write boundary in append, seal, and rotation.

For each scenario the expected open status, exact retained window, gap events,
and subsequent writability are defined in advance and asserted. An
`unrecoverable` result is acceptable only for a case whose authoritative
volume/geometry metadata was explicitly destroyed; it is not an alternative
oracle for ordinary tail faults. Sparse streams are mandatory: a record smaller
than a block followed only by a checkpoint must become durable.

### Integration tests

- regular fixed-size files through real `pread/pwrite/fdatasync`;
- reopen after process termination at scripted failpoints;
- multiple readers while the writer rotates repeatedly;
- an external CLI reader racing rotation and reporting possible gaps;
- configuration changes taking effect only at a new partition;
- tool smoke tests for format, inspect, verify, dump, and load generation;
- optional loop-device/raw-device tests in privileged CI/hardware labs.

### Fuzz/sanitizer tests

All decoders accept attacker-like/corrupt bytes and must fail without out-of-
bounds access or unbounded allocation. CI profiles should run ASan+UBSan and a
coverage-guided fuzzer over volume/partition/block/compression decoding. These
tools are optional build-time dependencies, not runtime library dependencies.

## 2. Workload profiles

The load generator uses deterministic seeds and reports input bytes, host write
bytes/calls, flushes, blocks, partitions, compression ratio, query bytes read,
latencies, and recovery bytes/time. Append/checkpoint percentile memory is a
deterministic reservoir capped at 65,536 samples per stream; observation count,
sample count, and the exact observed maximum are reported separately. The
measured query is one record-decoding pass, not a validation scan plus a query.
Its timer and backend counters stop before the separate deterministic
regeneration pass; the persisted-write observer records only compact block
metadata in the measured write path. It reassembles successful short-write
fragments before interpreting a frame; `--backend-write-chunk N` forces this
path through the normal CLI smoke suite.

It does not retain the complete input stream in RAM. Payload size/content are
pure functions of `(seed, ordinal)` and every payload carries its ordinal.
Verification regenerates every retained record, compares its complete framed
bytes, selector, time and flags, and requires a nonempty contiguous physical
suffix ending at the final submitted ordinal. A bounded storage wrapper below
the ring independently observes successful partition-header and block-frame
writes, models slot replacement, and derives the exact expected retained
first/count/block layout without consulting the catalog, index, `inspect()`, or
query code. Both the pre-close and reopened stream must match this write-set
oracle, including first/last/count/block identity, raw bytes, and rolling
digest.
The concurrent profile applies the same regeneration oracle to every data event
seen while rotation is active, so a reader loop cannot pass vacuously.

| Profile | Purpose |
|---|---|
| tiny-steady | 8--32 byte records at a stable rate; metadata and packing cost |
| mixed-telemetry | mostly small records with 1% in the 50--150 byte range |
| burst | high peak followed by idle; queue/backpressure and checkpoint tail |
| sparse | one/few records per durability interval; worst partial-block overhead |
| compressible | repeated/zero-heavy binary and text logs; codec benefit |
| incompressible | deterministic pseudo-random payload; fallback overhead |
| disordered-time | bounded regressions, duplicates, large jump, 1970-to-synced |
| hot-rotation | very small test partitions; generation/race behavior |
| concurrent-read | several range readers during sustained write/rotation |
| log-burst | variable text lines, severity/components, occasional large record |

## 3. Metrics and acceptance gates

Initial software gates, independent of a particular project data rate:

- all unit/model/fault/integration tests pass under normal and sanitizer builds;
- every persistent decoder has malformed/truncated/overflow tests;
- every modeled ordinary append/seal/rotation crash preserves the exact durable
  retained baseline and returns only the explicitly materialized valid
  extension; destruction of required volume metadata has a separately asserted
  error status;
- a checkpointed record survives modeled crash in 100% of crash positions after
  the successful flush boundary;
- a non-checkpointed recovery result never claims durability and contains only
  the exact whole-frame extension allowed by that crash image;
- time-index property tests produce zero false-negative blocks;
- stale-generation property tests produce zero silently returned stale blocks;
- a compressed representation is selected only when smaller, so a codec never
  expands a block on media;
- measured host write amplification and metadata ratio are reported for every
  workload rather than hidden by an average;
- resident buffers remain within the documented formula for the configured
  block, active index cache, and async queue sizes.
- production-source coverage stays at or above 85% of executable lines and 48%
  of individual branch outcomes taken. Per-file floors cover the POSIX backend,
  memory backend, codecs, persistent format, ring, record profile, and async
  writer independently. `Branches executed` is reported only as a diagnostic
  because it proves evaluation of an expression, not both outcomes.

Project qualification adds hardware gates:

- sustain at least the measured project peak rate with agreed CPU headroom;
- measure p50/p95/p99/max append, checkpoint, query, and recovery latency;
- verify worst checkpoint completion latency on the exact eMMC/NAND/controller
  and filesystem/block-device configuration;
- calculate lifetime host writes and conservative device write amplification
  against rated endurance with margin;
- run repeated physical power cuts and compare durable sequence markers;
- record kernel/filesystem/mount/cache configuration with the result.

The configurable loss target is not claimed from `sync_interval` alone. The
evidence is:

`maximum accepted-to-durable age = scheduling delay + queue residence + block publication + fdatasync latency`

and must be below the project target under its qualified worst-case load.
Both synchronous and asynchronous APIs expose accepted/durable watermarks,
sync duration, queue residence, and accepted-to-durable high-water metrics. A
failed flush increments errors and never advances a durable watermark.
The async wrapper accepts an optional monotonic nanosecond clock for deterministic
deadline tests; advancing it and calling `notify_clock_advanced()` drives the
same deadline logic used with `steady_clock` in production. Tests stall the
backend exactly at flush and assert `interval + stall`, then repeat with a
failed flush and require zero false durability. Boundary regressions reject an
interval whose nanosecond representation cannot fit and drive the clock to
`UINT64_MAX`, proving exactly one timer checkpoint rather than a busy-loop.

## 4. Reproducible commands

```sh
make check          # library tests and CLI black-box tests
make sanitize       # ASan + UBSan
make coverage       # instrumented tests and production-source summary
make crash-matrix   # bounded deterministic crash/fault suite
make tsan           # native-Linux ThreadSanitizer target
make benchmark      # deterministic workload profiles
```

The mock proves host call ordering and logical crash invariants. Only physical
power-cut testing can establish that a particular controller honors flushes and
characterize its internal write amplification.

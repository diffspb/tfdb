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

The C++ test executable deliberately remains one translation unit with one
tiny dependency-free registry, but its scenarios are split by concern under
`tests/cases/`: format/codec, storage/recovery, query/rotation, I/O faults,
records/readers, and async/integration. This keeps static registration simple
while preventing one 3000-line review unit. Build frontends must track changes
to those included scenario files.

The Rust reader has module unit tests plus corpus and recovery integration
tests. These are independent of C++ execution; the separate cross-language
test then proves agreement against the same bytes.

The shared manifest currently has 18 cases. Ten full images are committed and
protected by `testdata/format-v1/SHA256SUMS`; the remaining eight compact
single-byte/truncation mutations are derived deterministically by the
cross-language harness. The committed negative and transition images cover
unknown feature semantics, checked region/volume bounds, a two-crash stale
writer-incarnation chain, and three snapshots across live partition reuse.
Both readers must agree on contract-level results and on the physical block
stream for every valid snapshot.

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
bounds access or unbounded allocation. These tools are optional build-time
dependencies, not runtime library dependencies.

`make fuzz` runs the repeatable media fuzzer in `tests/fuzz_media.cpp`. It
derives images from the committed conformance corpus and drives each one
through lazy open, strict open, `inspect()`, `scan_blocks()`, and
`query_records()` across several time domains. Mutation is biased toward the
structural prefix — both volume header copies, the partition header regions,
and the first block frames — because uniform byte flipping rarely reaches a
length or offset field. Shapes are single-bit flips, byte replacement, and
sector-sized splats that model a torn or reused sector, plus truncation at a
random length.

The oracle is deliberately mechanical: no image may crash, hang, trip a
sanitizer, or yield a status code outside the documented set. Whether a
particular corrupted image *should* have remained readable is decided by the
shared corpus manifest in `testdata/format-v1/`, not by a random mutator. The
harness always exercises the unmodified corpus first, so a broken harness fails
immediately instead of after a long clean run.

Runs are reproducible from `(corpus, seed, iteration)`, and the target always
builds its own ASan/UBSan binary because the sanitizers are the detector:

```sh
make fuzz                            # quick regular run, 20,000 iterations
make fuzz FUZZ_ITERATIONS=500000     # long run
make fuzz FUZZ_JOBS=$(nproc)         # shard across independent processes
make fuzz FUZZ_SEED=$(date +%s)      # explore a new seed
make fuzz FUZZ_TIMEOUT=3600          # bound the wall clock per shard
```

`FUZZ_JOBS` shards the run across processes rather than threads. Each shard
gets its own seed, derived as `FUZZ_SEED + shard * FUZZ_SEED_STRIDE`, so shards
share nothing, `(seed, iteration)` still identifies an image exactly, and a
sanitizer abort stays contained in the shard that hit it instead of killing the
whole run. Threads would add no throughput, because iterations already share no
state, and would make a failure ambiguous between a defect in the library and a
race in the harness — this fuzzer's oracle is about media decoding, not
concurrency. Concurrent use of independent stores is safe regardless: the
library holds no mutable global state, and its only statics are an immutable
CRC table and immutable built-in codecs.

`FUZZ_ITERATIONS` is the total across shards, so changing `FUZZ_JOBS` changes
wall time rather than how much work is done. Measured on a 20-logical-core
WSL2 host, 60,000 iterations took 29.4 s at `FUZZ_JOBS=1` and scaled to 2.0x,
3.6x, 6.5x, and 8.0x at 2, 4, 10, and 20 shards; scaling flattens past roughly
half the logical cores because the sanitized workload is memory-bound.

A finding prints its seed and iteration, writes the offending image under
`FUZZ_ARTIFACTS` (default `build/fuzz`), and prints the replay command. With
several shards, each one also names its own log, and any failing shard fails
the whole run:

```sh
build/fuzz/tfdb_fuzz_media --image build/fuzz/tfdb-fuzz-SEED-ITERATION.tfdb
```

Regular use means a fixed seed on every change and a rotating seed on a
schedule, so the corpus is explored more widely over time while any single
finding stays reproducible. A saved artifact that reveals a real specification
gap belongs in the shared corpus with an expected classification, not only in
the fuzz harness.

Coverage-guided fuzzing over the individual decoders remains open work; this
harness covers whole recovered volume images and needs no external fuzzing
runtime.

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
make fuzz           # ASan/UBSan media fuzzer over derived corpus images
make cxx17-check    # C++17 rebuild proving no Status is left unchecked
make tsan           # native-Linux ThreadSanitizer target
make benchmark      # deterministic workload profiles
make rust-test      # independent reader unit/corpus/recovery tests
make conformance    # regenerate corpus and compare C++/Rust outcomes
make build-system-test # CMake/Meson build, install, and external consumers
```

Alternative frontend checks are:

```sh
cmake -S . -B build/cmake -G Ninja
cmake --build build/cmake
ctest --test-dir build/cmake --output-on-failure

meson setup build/meson
meson compile -C build/meson
meson test -C build/meson --print-errorlogs
```

Before a format-freeze candidate, also run `cargo fmt -- --check`, generate
`cargo doc --no-deps`, regenerate the shared volume, verify its committed
SHA-256, and have a reviewer compare the Rust behavior to `format-v1.md`
without using C++ internals as the source of truth.

The mock proves host call ordering and logical crash invariants. Only physical
power-cut testing can establish that a particular controller honors flushes and
characterize its internal write amplification.

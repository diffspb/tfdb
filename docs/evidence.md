# Reference verification evidence

This file records reproducible software evidence for the current C++14
candidate. It is not a production eMMC/NAND qualification certificate.

## LZ4 block codec evidence (2026-09-09, branch feature/lz4-block-codec)

These checks ran on the same WSL2 kernel family with g++ 13.3.0 and
rustc/cargo 1.98.1. They cover `lz4_block:1`, the LZ4 raw block format now
implemented behind the existing `CompressionCodec` interface in the C++ writer
and reader and decode-only in the Rust reader.

- The C++ suite is 85/85 (was 81/81). The four added tests cover the normative
  byte vectors and the malformed streams the grammar rejects, round trips over
  every size across the thirteen-byte match threshold plus payload-limit
  neighbours and a match beyond the 65535-byte window, encoder determinism
  across repeated calls and factory instances, every truncation and every
  single-bit mutation of a valid stream, 20,000 arbitrary bounded inputs, and a
  partition that stores both compressed and incompressible blocks.
- The Rust crate runs 21 tests (was 17). The added ones decode the shared
  `lz4_block:1` image to the exact payloads the C++ writer compressed, and hold
  the decoder to every truncation and single-bit mutation of a valid stream.
- The shared corpus is 20 manifest cases over 12 committed images. The added
  `codec-lz4-block.tfdb` carries four blocks: a 2000-byte run stored in 18
  bytes, an extended-literal payload stored in 59, a 1500-byte cyclic payload
  stored in 275, and a pseudo-random payload the codec could not shrink, which
  stores itself as `none:1` inside the compressed partition. `make conformance`
  regenerates it byte for byte and both readers produce identical block output.
  `codec-unknown-version.tfdb` is the same volume claiming `lz4_block:2`; both
  readers call it unsupported at open rather than assume version 1 is close
  enough.
- The encoder's output is byte-identical across `-O0`, `-O2`, `-O3`,
  `-fno-inline`, and an ASan/UBSan build: each regenerates
  `codec-lz4-block.tfdb` byte for byte. A 32-bit build was not exercised here
  for lack of multilib headers, which is precisely the divergence that ruled
  out the reference encoder, so the property is carried by construction: the
  search reads four bytes little-endian by explicit shifts and indexes a table
  by input offset, with no dependence on pointer size or host word order.
- `make fuzz FUZZ_ITERATIONS=5000000 FUZZ_JOBS=20 FUZZ_SEED=1788904486` ran
  5,000,000 iterations across 20 shards with no crash, hang, sanitizer report,
  or undocumented status. The harness now derives images from both the
  `valid-mixed` and `codec-lz4-block` corpora, and each iteration additionally
  mutates a valid codec stream, seeded from the encoders themselves, and drives
  every built-in decoder over it.
- `make check`, `make crash-matrix`, `make sanitize`, and `make cxx17-check`
  are clean with zero warnings under the standard warning set. Coverage holds:
  `codec.cpp` is 97.0% of lines and 68.0% of branch outcomes against 85%/50%
  floors, and the production total is 92.7% of lines.

### Review against the reference implementation

The implementation was written rather than vendored (ADR-037), so it was
checked in both directions against liblz4 4.4.5 through the `lz4` Python
bindings. This harness is not part of the repository; the numbers below are the
justification for the ADR, not a build dependency.

- 87 payload cases, from empty through 140 kB, each compressed by TFDB and
  decompressed by liblz4, and compressed by liblz4 and decompressed by TFDB.
  All 87 round-tripped both ways. TFDB's encoder produced 228,926 bytes against
  liblz4's 222,751 for the same inputs, so the simple single-table search costs
  about 2.8% of ratio.
- 400,000 mutated, random, and structured-random streams under ASan/UBSan.
  TFDB never accepted a stream liblz4 rejected, never disagreed on the decoded
  bytes of a stream both accepted, and rejected 15 of the 5,411 streams liblz4
  accepted. All 15 fall under the two rules `docs/format-v1.md` names: 13 were
  a nonzero match nibble on the final token and 2 were an invalid match offset.
  Offset zero is the interesting one: liblz4 copies bytes onto themselves and
  returns a full-size, plausible result rather than an error.

### Measured effect on the first real consumer

The 60 independently compressed chunks exported by telezip
(`build/telezip_export --chunks chunks.bin`, 742,908 bytes), each run through
the block layer's rule that compressed bytes are stored only when strictly
smaller:

| codec | stored bytes | ratio | blocks compressed |
|---|---:|---:|---:|
| `none:1` | 742,908 | 1.0000 | 0/60 |
| `packbits:1` | 737,151 | 0.9923 | 60/60 |
| `lz4_block:1` | 631,400 | 0.8499 | 60/60 |

Every chunk round-tripped under every codec. This confirms the 15.0% figure
that motivated the work, and slightly beats the 631,884 bytes the task document
measured with `pyarrow`'s `lz4_raw`. Encode and decode times on this host were
in the single-digit milliseconds for all 60 chunks but varied by more than 5x
between runs under load, so they are not quoted as a result. The number is a
property of telezip's payload, not of TFDB: an application writing already
compressed records would gain nothing, which is why the codec stays optional
and per-partition.

### Not covered here

- `make build-system-test` could not complete on this host. CMake configures,
  builds, passes both ctests, installs, and satisfies its external consumer;
  Meson does the same up to its external consumer step, which needs a
  `pkg-config` binary that is not installed. No build definition changed,
  because the codec added no source file.
- `make tsan` and the long reader/writer soak remain native-Linux gates.
- The decision about whether telezip enables block compression by default is
  deliberately separate and needs the real vehicle traffic, not this corpus.

## Post-baseline interoperability evidence (2026-09-06)

The following worktree checks ran on the same WSL2 kernel family with g++
13.3.0, CMake 3.28.3, Meson 1.3.2, Ninja 1.11.1, and rustc/cargo 1.75.0. These
tools are build/test dependencies only; neither the C++ runtime nor the Rust
reader has a third-party runtime/library dependency.

- The original 3015-line C++ test source is now a 260-line runner/support file
  plus six thematic scenario files of 320--514 lines. The same 76/76 tests pass.
- CMake/Ninja and Meson/Ninja each build the library, five C++ tools, two
  examples, the test runner, and deterministic conformance writers; each runs
  its unit and CLI smoke tests successfully. Both install to an isolated
  prefix, and standalone consumers build using only installed CMake package
  metadata or Meson-generated `tfdb.pc`, public headers, and `libtfdb.a`.
- The Rust crate runs 17 tests: four codec/CRC/profile unit tests, three
  decoder-robustness tests, five shared-volume conformance tests, and five
  recovery/corruption tests. The decoder tests cover every truncation of each
  persistent structure, every single-bit mutation of CRC-protected corpus
  structures and the first record block, and 10,000 deterministic bounded
  arbitrary inputs without panics.
- `make conformance` regenerates all ten committed shared images byte-for-byte,
  checks their committed SHA-256 manifest, compares C++/Rust block output for
  every valid image plus base-volume inspect and exact record-query output, and
  requires matching classifications for all 18 manifest cases. The cases add
  unknown feature semantics, feature/volume bounds, a two-crash stale-writer
  chain, and three live-rotation snapshots.
- The required long media-fuzz gate ran 5,000,000 derived-image iterations
  under ASan/UBSan as 20 independent shards of 250,000 iterations, base seed
  `202609062200` and stride `1000003`. It completed without a crash, hang,
  sanitizer report, undocumented status, or saved failing image.
- The valid corpus contains four blocks, eight FramedRecordV1 records, 714 raw
  bytes, one sealed PackBits partition, one footerless raw partition, two time
  domains, disordered time, unsynchronized time, and an anomaly flag.
- `cargo fmt -- --check`, `cargo check --all-targets`, and
  `cargo doc --no-deps` pass; the 15-page committed HTML documentation mirror
  is current and all local links/anchors validate.

The expanded corpus exposed one classification-order difference: Rust rejected
an unknown optional feature before checking its overflowing media range, while
C++ classified the range as structural damage and exposed a read-only header
gap. ADR-035 fixes the validation order and both readers now agree.

This closes the named initial corpus expansion, not independent review or the
format freeze. Native-Linux fuzz/TSan/soak, trace replay, target power
cuts/endurance, and a pilot remain open as listed in
[`roadmap.md`](roadmap.md). The historical benchmark digest and measurements
below are preserved rather than relabeled as evidence for this changed
worktree.

## Low-risk hardening evidence (2026-09-06, branch maintenance/low-risk-hardening)

Same host and toolchain as the section above, plus CMake 4.4.3, Meson 1.12.0,
and Ninja 1.13.2 from a disposable virtualenv. Reproduced on this worktree:

- `make check`: **81/81** library/recovery/integration tests and the CLI smoke
  tests pass. Five are new: the version-consistency check, the incremental
  CRC32C equivalence check, the async thread-exception guard, and two
  writer-health cases;
- `make crash-matrix`: all **14** bounded recovery groups pass;
- `make sanitize`: 81/81 tests and tool smoke pass under ASan and UBSan;
- `make coverage`: **92.3%** production line coverage (2526 executable lines),
  50.8% of 3607 branch outcomes; every aggregate and per-file floor passes.
  `src/version.cpp` was added to the gated source list so a new production
  source cannot silently escape the gate;
- `make cxx17-check` (new): the library, tools, examples, and tests rebuild as
  C++17 with `-Werror` and leave no `Status` unchecked;
- `make fuzz` (new): **5,000,000** derived-image iterations against
  `testdata/format-v1/valid-mixed.tfdb` under ASan and UBSan produced no crash,
  hang, sanitizer report, or undocumented status code, and saved no artifact.
  Run as 20 shards from base seed 20260906, so the shard seeds were
  20260906 + n * 1000003 for n in 0..19, 250,000 iterations each, in 303 s
  wall clock. A single-process 500,000-iteration run at seed 20260906 passed
  the same way beforehand. This is a whole-image mutation fuzzer, not a
  coverage-guided one; that gate stays open;
- fuzz sharding scales as measured over 60,000 iterations on this
  20-logical-core host: 29.4 s at one job, then 2.0x, 3.6x, 6.5x, and 8.0x at
  2, 4, 10, and 20 shards. Scaling flattens past roughly half the logical
  cores because the sanitized workload is memory-bound;
- `make build-system-test`: CMake configure/build/CTest/install and the
  installed-package CMake consumer pass; Meson configure/compile/test/install
  pass. The final Meson consumer was subsequently verified during independent
  merge review with pkg-config 1.8.1;
- `add_subdirectory()` consumption was checked directly: a parent project gets
  only the `tfdb` target, no `BUILD_TESTING` option, and no TFDB install rules.
  `-DTFDB_POSITION_INDEPENDENT_CODE=ON` adds `-fPIC` and the default does not;
- the shared conformance volume still regenerates byte-for-byte to SHA-256
  `7d81004d396903b4f7194a5635f31e11fbf4664ca2fa23b38a5e5588e5bac254` after the
  CRC32C refactor, which is the evidence that no encoded byte changed;
- `python3 docs/build_html.py --check` validates all 15 pages.

Two measurements motivated changes rather than recording them:

- heap allocations during a 20,000-record query fell from 24,041 to 3,330
  (about 86% fewer) once CRC verification stopped copying each structure and
  each record;
- removing the exception guard from `AsyncWriter::run()` makes the test binary
  abort with `terminate called after throwing an instance of
  std::runtime_error` and dump core, which is the failure the guard prevents.

Independent merge review re-ran all 81 tests, the 14-group crash matrix,
ASan/UBSan, coverage, C++17, both build/install/consumer frontends, all 14 Rust
tests, and cross-language conformance successfully. A post-correction short
fuzz run covered 20,000 images in four shards from base seed 9072026 without a
crash, hang, sanitizer report, or undocumented status. The correction only
strengthened publication ordering between related writer-health atomics and
did not change encoded bytes. `make tsan` remains unclaimed for the reason
recorded below.

## Test and analysis results

Reference environment on 2026-09-01:

```text
Linux 6.6.87.2-microsoft-standard-WSL2 x86_64
g++ 13.3.0 (Ubuntu 24.04 toolchain)
source bundle SHA-256 e89903d93b09511ac3b49a3ff4107eb75efd34d600881b3c31f741ca7435e067
```

The digest covers `Makefile`, `benchmarks` except reference artifacts,
`include`, `src`, `tests`, and `tools`. Documentation, skills, and standalone
consumer examples are checked separately. The unchanged benchmark-command stdout is retained
in [`benchmarks/reference-2026-09-01.raw.log`](../benchmarks/reference-2026-09-01.raw.log);
the digest scope and derived summary are in
[`benchmarks/reference-2026-09-01.txt`](../benchmarks/reference-2026-09-01.txt).
The 39,528-byte raw artifact SHA-256 is
`d8aa2c1b4ac968e966fc043192b7d0c70e0ec11b4a1f36c8eb11be4082d08124`.

Current results:

- `make check`: 76/76 library/recovery/integration tests passed and all five
  command-line tools passed black-box smoke tests;
- `make crash-matrix`: all 14 bounded recovery groups passed;
- `make sanitize`: 76/76 tests and tool smoke tests passed under ASan and
  UBSan; leak detection was disabled because LeakSanitizer cannot operate in
  this traced environment;
- `make coverage`: 92.2% production-source line coverage (2454 executable
  lines), 79.6% branch expressions evaluated, and 50.7% of 3571 individual
  branch outcomes taken. The passing gates are 85% aggregate lines and 48%
  aggregate outcomes, plus explicit per-file floors; notably `ring_store.cpp`
  is 93.3%/51.3%, `internal_format.cpp` 91.9%/46.1%, and
  `async_writer.cpp` 97.3%/62.2% lines/outcomes;
- `make -C examples check` passes both complete C++14 consumers; both also pass
  with ASan/UBSan and when rebuilt outside the repository against a staged
  prefix containing only public headers and `libtfdb.a` with `-Werror`;
- `python3 docs/build_html.py --check` validates all 12 offline HTML pages,
  source hashes, local links, and anchors; the TFDB integration skill passes
  the skill-creator structural validator;
- `make tsan`: compilation succeeds, but the GCC ThreadSanitizer runtime aborts
  before the tests on this WSL2 kernel with `unexpected memory mapping`. The
  target remains mandatory for native-Linux CI and is not claimed as passed;
- warning-clean C++14 builds use `-Wall -Wextra -Wpedantic -Wconversion
  -Wshadow -Wformat=2 -Wnull-dereference -Wdouble-promotion`.

The suite includes:

- independent normative bytes for volume, partition, block, index, footer,
  PackBits, and the optional framed-record envelope;
- all 130 persistence cuts of a 129-byte uncheckpointed raw frame and all 133
  cuts of a selected 132-byte PackBits frame;
- every complete subset of three dirty block writes, reverse persistence order,
  header-only and torn-second cases, with an exact longest-prefix oracle;
- none/torn/complete persistence after a failed flush, proving that a failed
  flush never advances the watermark even if a controller persisted bytes;
- hard failures at every write and flush of clean and dirty seal/rotation
  transitions, with an exact retained-window oracle and writable reopen;
- index/footer none, prefix, subset, and both write orders; torn new headers in
  unused and reused slots; and the backend-write/RAM-update boundary;
- short I/O continuation, zero progress, interruption, hard read/write error,
  and failed-flush watermark behavior; the CLI write-set oracle is also run
  over forced 17-byte backend write fragments;
- public codec/profile boundaries reject invalid nonempty byte views and
  safely permit documented input/output aliasing; writer
  readiness distinguishes read-only, closed, and original fault states, and
  the minimum one-quantum data geometry matches the format specification;
- lazy versus strict corruption detection, damaged volume/header/index cases,
  footerless historical-partition rescan corruption, valid-CRC but false footer
  summaries, refusal to write through an ambiguous damaged current header,
  mandatory record CRC, exact feature/config layout, and registered codec
  validation;
- 10,000 deterministic block-header mutations, every decoder truncation
  length, and 10,000 malformed PackBits inputs under bounds/sanitizers;
- a two-crash stale-suffix scenario prevented by the writer-incarnation chain;
- disordered/negative/unsynchronized time, domain isolation, typed automatic
  rotation, selector filtering, and anomaly-reset behavior;
- a deterministic overwrite between block-header and payload reads, plus four
  concurrent readers during 100 hot-rotation writes;
- async FIFO durability barrier, exact crash/reopen after drain, deterministic
  queue backpressure under a backend stall, and a custom-clock timer oracle
  proving `interval + stall` age. A timer flush failure never claims
  durability; unrepresentable intervals are rejected and a clock reaching
  `UINT64_MAX` checkpoints once without looping. Checked profile/domain
  propagation and background errors are also covered;
- load generation uses bounded latency reservoirs and a retained-write model;
  it regenerates every retained record from `(seed, ordinal)`. A wrapper below
  the ring models successful block writes and slot replacement, and both live
  and reopened streams must match its exact first/count/block/digest result;
- regular-file reopen and text-log payload/query paths.

## Exact host-I/O facts from the mock

For v1.0 with no injected short I/O:

| Operation | Backend writes | Flushes | Bytes requested |
|---|---:|---:|---:|
| format volume headers | 2 | 1 | 512 |
| start a partition generation | 1 | 1 | 1024 |
| checkpoint one 1-byte raw block | 1 | 1 | 129 |
| repeat a clean checkpoint | 0 | 0 | 0 |
| seal one already-checkpointed one-block partition and create next | 3 | 2 | 48 index + 256 footer + 1024 header |

Frame padding up to `persistence_quantum` is address-space reservation and is
not written. A short successful backend write continues exactly at its
remaining offset. These are host requests: filesystem journal traffic and
controller/FTL/NAND writes are deliberately not inferred from them.

## Reference workload results

Three sequential repetitions, reproducible with `PROFILE_RECORDS=50000
PROFILE_REPEATS=3 make benchmark`, used seed 20260901. The normal geometry was a
16,785,408-byte file, four 4 MiB partitions, 256 KiB index reserve, 32 KiB
blocks, 4 KiB persistence quantum, 50,000 records, and a checkpoint every
1,000 records. Sparse used 2,000 records and checkpointed every record.
Hot-rotation and concurrent-read used a 204,800-byte volume, 64 KiB
partitions, 256-byte blocks, 5,000 records, and 118 rotations.

Throughput below is the median of three runs. Latency is the worst observed
maximum, intentionally preserving scheduler/storage outliers. Recovery is the
median. All values are reference WSL2 virtual-disk measurements, not targets.

| Profile | Median records/s | Host bytes/framed input | Blocks/rotations | Worst sync | Worst accepted→durable | Median recovery |
|---|---:|---:|---:|---:|---:|---:|
| tiny | 414,546 | 1.00492 | 100/0 | 6.91 ms | 7.72 ms | 4.34 ms |
| mixed telemetry | 400,628 | 1.00412 | 100/0 | 3.79 ms | 4.63 ms | 5.22 ms |
| repeated bytes, PackBits | 394,969 | 0.21707 | 250/0 | 6.09 ms | 6.97 ms | 2.99 ms |
| pseudo-random, PackBits | 284,288 | 0.95949 | 250/2 | 9.62 ms | 11.58 ms | 0.34 ms |
| text logs, PackBits | 224,200 | 0.97877 | 393/3 | 12.28 ms | 14.38 ms | 0.20 ms |
| bad/disordered time | 396,716 | 1.00400 | 100/0 | 4.77 ms | 5.63 ms | 5.34 ms |
| async burst, 50 ms timer | 625,195 | 1.00518--1.00520 | 159--160/1 | 13.71 ms | 64.09 ms | 2.83 ms |
| sparse checkpoint-every-record | 649 | 4.35685 | 2000/2 | 10.97 ms | 10.97 ms | 0.09 ms |
| hot rotation | 9,953 | 2.35609 | 1546/118 | 7.32 ms | 7.34 ms | 0.03 ms |
| concurrent read + hot rotation | 9,876 | 2.35609 | 1546/118 | 4.43 ms | 4.46 ms | 0.03 ms |

The measured query is exactly one decoded-record pass; strict regeneration is
performed afterward and excluded from its timer/backend counters. Median query
times were 11.39 ms for tiny (2.82 MB read), 50.58 ms for logs (12.17 MB), and
0.286 ms for the 121-record hot-rotation retention window (94,346 bytes).

The concurrent workload completed 288--326 full reader scans, validated
9,156--10,386 data events and 29,542--33,546 records per repetition without
corruption or unexpected gaps while the writer rotated 118 times. Every live
record, including its flags and complete framed bytes, was regenerated from
`(seed, ordinal)` and compared byte-for-byte. The
deterministic unit test forces the precise overwrite race and verifies one
explicit `overwritten_gap`; a benchmark need not manufacture one to pass.

For async burst, the worst submit-to-durable observation was 64.09 ms, the
worst queue residence was 20.19 ms, and unbounded producers saw 0--53,066
backpressure responses with queue capacity 8192; one scheduling run did not
saturate the queue. This supports
the contract
`timer + queue residence + publication + sync`; it also proves why the timer
alone cannot be advertised as the loss bound.

PackBits reduced the repeated-byte profile to 0.217 host bytes per framed input
byte, but barely helped logs (0.979); pseudo-random input remained at 0.959
including metadata and raw fallbacks. Sparse durability is intentionally
costly: checkpointing each tiny record produced 4.36 host bytes per framed input byte.
Those explicit extremes are more useful than one blended amplification number.

## What remains before a product claim

For every target project and exact hardware/software image:

1. replay representative CAN/Ethernet telemetry and logs, including peak bursts
   and clock synchronization events;
2. set the checkpoint interval and queue capacity, then require measured
   maximum accepted-to-durable age below the chosen loss target with margin;
3. perform automated physical power cuts at random and targeted
   append/seal/rotation phases and compare durable sequence markers;
4. test the selected filesystem/mount/cache configuration and raw-device path
   wherever each is deployed;
5. record p50/p95/p99/max sync, recovery, query, CPU, RSS, and host writes;
6. combine device endurance/retention data with conservative controller write
   amplification and prove the eight-year margin;
7. repeat power-cut and retention tests at temperature, low voltage, high wear,
   and near-full lifetime conditions;
8. pass TSan and a long multi-reader soak on native Linux;
9. validate all golden/corruption vectors with the independent Rust reader
   before freezing format v1.

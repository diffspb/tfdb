# Post-1.0 roadmap and project handoff

This is the canonical continuation plan for TFDB (Telemetry Flash DB). It is
written so development can resume on another machine without relying on chat
history. The architecture and evidence documents remain authoritative for
their respective subjects.

## 1. Current baseline

The immutable source-release baseline is:

```text
library release: 1.0.0
Git tag: v1.0.0
Git commit: 353ee2b
language delivered: C++14
runtime dependencies: none beyond Linux/POSIX and the C++ standard library
```

The baseline contains the synchronous ring store, bounded asynchronous
wrapper, regular-file/block-device backend, deterministic memory/fault model,
FramedRecordV1, raw and PackBits blocks, time indexing, query/gap handling,
five diagnostic/load tools, examples, and Markdown/HTML documentation.

Reference software evidence is reproducible and recorded in
[`evidence.md`](evidence.md). It includes 76 library/recovery/integration tests,
14 focused crash-matrix groups, ASan/UBSan, line/branch coverage gates, public-
header consumers, and deterministic workload profiles. Those results describe
the recorded WSL2 environment only.

Post-baseline work now present in the repository adds CMake and Meson build
frontends, splits the single large test source into six thematic scenario
files, and delivers the independent standard-library-only Rust reader and
three read-only tools. `testdata/format-v1/valid-mixed.tfdb` is regenerated
byte-for-byte by the C++ public API; the cross-language test compares both
implementations over its valid and derived corruption/recovery cases. See
[`rust-reader.md`](rust-reader.md) and [`evidence.md`](evidence.md).

The current post-baseline development version is 1.1.0 and is distributed
under the BSD-2-Clause license. This does not move or rewrite the immutable
`v1.0.0` tag, declare a 1.1.0 release, or freeze the persistent format.

### Two version axes

Do not confuse the library release with the persistent-format lifecycle:

- `1.0.0` is the first C++ source release;
- format v1 is still a candidate until the independent reader and hardware
  qualification gates below pass.

Library releases may advance without changing the on-media major. Conversely,
an incompatible media change requires an explicit format decision and cannot
be hidden inside a library patch release. Until format freeze, test volumes are
not promised as permanent cross-project archives.

## 2. Next milestone

The next milestone is **format-v1 qualification and freeze**. It is not a
feature milestone. Its purpose is to show that the written specification is
independently implementable and that the original durability, retention, and
flash-use objectives hold on an actual deployment stack.

The critical path is:

```text
shared conformance corpus
        -> independent Rust reader              [initial gate implemented]
        -> specification ambiguities resolved   [current corpus resolved]
        -> native-Linux verification and real-trace replay
        -> target hardware power-cut/endurance qualification
        -> pilot application evidence
        -> format-v1 freeze
```

Several workstreams can proceed in parallel, but format freeze requires every
gate in section 5.

## 3. Workstreams in priority order

### A. Release and laboratory hygiene

Complete the inexpensive repository work first:

1. **Complete:** BSD-2-Clause is the project license and retains Alexander
   Safronenko's copyright notice in source and binary distributions.
2. **Complete:** `CHANGELOG.md` records source/API releases separately from the
   on-media format lifecycle.
3. Add install/package support suitable for consumers while preserving the
   dependency-free runtime. A `DESTDIR`-aware Make target and optional
   pkg-config/CMake package metadata are sufficient; another build system need
   not own the project.
4. Establish CI on native Linux for GCC and Clang. Include x86-64 and the
   actual deployment architecture; ARM64 is common but must be confirmed for
   the target project.
5. Publish release archives from immutable tags and record compiler/platform
   metadata. Do not move `v1.0.0`.

Exit condition: a clean native-Linux checkout can build, test, install, and
consume only public headers plus `libtfdb.a` without repository-relative paths.

### B. Shared conformance corpus and independent Rust reader

Start Rust with a read-only implementation rather than duplicating the whole
writer. Its purpose is to challenge the specification independently.

1. Extract persistent golden bytes from C++-only test code into a versioned
   `testdata/format-v1/` corpus with a machine-readable manifest.
2. Include valid volume, partition, block, index, footer, PackBits, and
   FramedRecordV1 objects plus truncation, CRC, bounds, unknown-feature, and
   stale-generation cases.
3. Keep expected classifications at the contract level (`valid`, `corrupt`,
   `unsupported`, gap/overwrite semantics); diagnostic message text is not a
   cross-language ABI.
4. Implement fixed-width little-endian decoding, checked arithmetic, CRC32C,
   PackBits, generation ordering, partition recovery, block iteration,
   FramedRecordV1 decoding, and exact selector/time filtering in Rust.
5. Provide read-only Rust equivalents of `inspect`, `verify`, and `dump`.
6. Run both implementations against the same corpus. The Rust code must not
   link to the C++ parser or copy its internal in-memory structures.

Exit condition: every shared valid vector has identical observable meaning;
every invalid vector is rejected in the documented class; live C++-generated
volumes can be inspected and dumped independently.

Current state: the independent reader implements the listed decoding,
recovery, scan/query, profile, and tool behavior. The deterministic mixed
volume plus 17 additional cases have matching C++/Rust outcomes. Ten complete
images are committed with SHA-256 digests; the others are deterministic small
mutations. Feature-directory semantics, checked-arithmetic boundaries, a
writer-chain stale suffix, and three live-rotation snapshots are now shared
cases rather than Rust-only tests. Their first run exposed and resolved the
classification-order ambiguity recorded by ADR-035. Independent corpus review
still remains part of the format-freeze sign-off.

### C. Native-Linux model, sanitizer, and fuzz qualification

1. Extend the randomized reference model to combine append, checkpoint,
   explicit/automatic rotation, configuration transition, query, corruption,
   crash, reopen, and continued writing in one command sequence.
2. Add coverage-guided fuzz targets for every persistent decoder and for whole
   recovered volume images. Build-time fuzz dependencies must not leak into the
   runtime library.
3. Run ASan/UBSan and ThreadSanitizer on native Linux. Treat compiler success
   followed by a TSan runtime abort as an environmental failure, not a pass.
4. Run a long one-writer/multi-reader hot-rotation soak and record iteration,
   gap, corruption, CPU, RSS, and latency totals.
5. Cross-check x86-64 and ARM64 behavior and the shared media vectors.

Exit condition: no sanitizer findings, no model mismatch, no unclassified
decoder failure, no silent reader corruption, and no stale-generation
resurrection.

### D. Representative trace replay

Extend `tfdb_loadgen` with deterministic trace replay before selecting final
geometry or checkpoint intervals.

The input contract should carry complete encoded bytes or reproducible payload
descriptors, arrival/index time, selector, flags, source/service identity, and
optional source time. It must represent:

- steady CAN and Ethernet traffic;
- concurrent Linux services and startup bursts;
- the real size distribution, including the 50--150 byte upper tail;
- slightly disordered arrivals;
- unsynchronized 1970-era source time and later synchronization;
- log bursts and binary/NUL-containing log payloads;
- overload, backpressure, and storage-stall periods.

Do not commit proprietary or personal telemetry. Store an anonymized,
documented corpus or a deterministic generator plus a digest of the protected
source capture.

For every candidate geometry record throughput, CPU, RSS, block utilization,
host write calls/bytes, rotations, recovery/query time, queue residence,
checkpoint latency, and maximum accepted-to-durable age.

Exit condition: the first project's peak profile runs with agreed headroom,
and its checkpoint/queue policy stays inside the chosen loss budget in the
software environment.

### E. Target storage and physical power-cut laboratory

Qualify each exact combination of device, controller/firmware, kernel,
filesystem or raw-device path, mount/cache settings, and TFDB configuration.

The harness needs:

- an explicitly identified expendable target device;
- externally controlled power removal, not process kill as a substitute;
- durable ordinal/session markers in generated records;
- cuts targeted at append, partial-block checkpoint, index/footer seal,
  partition-header replacement, and rotation, plus randomized cuts;
- automated reopen, `verify`, exact retained-window comparison, and continued
  writing after recovery;
- current/voltage and temperature scenarios appropriate to the product;
- machine-readable run manifests and immutable raw results.

Required invariants are stronger than “the database opens”:

- no record covered by a successfully returned checkpoint disappears;
- recovered data is an exact physically ordered retained prefix/window allowed
  by the last successful durability boundary and subsequent whole valid frames;
- no stale overwritten generation reappears;
- gaps and unrecoverable authoritative-metadata damage are classified rather
  than hidden;
- writable recovery remains possible for every modeled ordinary tail fault.

Measure host and device/controller write amplification separately where the
platform exposes enough information. Combine measured rotation/write rates,
device endurance and retention ratings, controller amplification, temperature,
and safety margin into the eight-year calculation in [`sizing.md`](sizing.md).

Exit condition: the agreed power-loss bound and eight-year endurance margin are
demonstrated for the exact deployment image. Results from one eMMC/NAND part do
not qualify another.

### F. First production pilot

Integrate one real storage service using the public C++ API and record its
project-specific contract:

- record profile ID/version and payload schema registry;
- selector bit allocation for source, half-set, service, and message identity;
- arrival/index clock, source-clock preservation, time-domain IDs, and time-
  quality transitions;
- synchronous/async ownership and bounded `busy` retry/drop/backpressure
  policy;
- handling of original writer failure, corrupt/overwrite gaps, shutdown, and
  recovery;
- telemetry/log shared-volume decision and overload priority;
- metrics, alarms, health reporting, and export completeness semantics.

Use [`tutorial.md`](tutorial.md), [`api.md`](api.md), and the repository's
`tfdb-integration` skill. The integration must not format on normal startup or
retain borrowed callback views.

Exit condition: the pilot passes restart, overload, corruption/gap, graceful
shutdown, physical power-cut, and representative-load acceptance tests with an
operational recovery procedure.

## 4. Deferred until measurements justify them

Do not put these on the critical path merely because extension points exist:

- built-in LZ4 or another compression codec;
- project-specific value indexes;
- reader leases that can stall rotation;
- `O_DIRECT` or per-write synchronous I/O;
- encryption/authentication inside the core format;
- a general logging schema package;
- full Rust writer/API parity.

Revisit an item only with a workload, compatibility, security, or resource
requirement that the current design cannot meet. Record the decision and its
media/API effect before implementation.

After format freeze, implement the independent Rust writer and public API,
then run bidirectional C++-writes/Rust-reads and Rust-writes/C++-reads crash and
rotation tests.

## 5. Format-v1 freeze gates

Format v1 may be declared frozen only when all of the following are recorded:

- no unresolved format-affecting item remains in `docs/decisions.md`;
- C++ and independent Rust readers pass the shared valid/corrupt/unsupported
  corpus;
- combined state-machine, fuzz, sanitizer, native TSan, and long-soak gates
  pass;
- representative project replay establishes geometry, queue, and checkpoint
  settings with headroom;
- physical power cuts establish durability/recovery behavior on target media;
- the eight-year endurance calculation passes with an agreed safety margin;
- a real pilot validates integration and operational recovery;
- format/API compatibility policy and supported feature registry are published;
- the exact specification, vectors, evidence, and implementation revisions are
  tagged together.

Any incompatible persistent change after that point requires a new on-media
major or an explicitly specified compatible feature mechanism.

## 6. Moving development to another computer

### Transfer

Push the branch and annotated tags to the chosen private remote, or create a
portable Git bundle when no remote is available:

```sh
git status --short
git bundle create ../tfdb.bundle --all
git bundle verify ../tfdb.bundle
```

On the new machine, verify the baseline before beginning work:

```sh
git log -1 --decorate --oneline v1.0.0
git show --no-patch --format=fuller v1.0.0
test "$(tr -d '\n' < VERSION)" = "1.0.0"
make check
make crash-matrix
make sanitize
python3 docs/build_html.py --check
```

The HTML generator additionally requires exactly `markdown-it-py 3.0.0`.
Native TSan, Rust, fuzzing, and hardware-lab dependencies are development/test
requirements only and must not become TFDB runtime dependencies.

### Recommended branch sequence

1. `qualification/conformance-corpus`
2. `rust/read-only-v1`
3. `qualification/native-linux`
4. `tools/trace-replay`
5. a hardware-specific qualification branch with no secrets or destructive
   defaults
6. a project-specific pilot integration outside the generic core where
   possible

Keep raw evidence tied to the source, configuration, hardware identity, and
command that produced it. Add new platform-specific evidence instead of
overwriting the WSL2 reference artifact.

### Safe restart point

Work that can begin without the hardware laboratory:

1. maintain the BSD-2-Clause license and release/format compatibility policy;
2. independently review the shared negative corpus and extend it when review
   or fuzzing exposes a specification gap;
3. configure native-Linux CI, TSan, fuzzing, and long soak;
4. define and anonymize the first replay corpus;
5. design the power-cut harness and acceptance manifest without touching a
   real block device.

No current task requires changing the persistent format. If independent
implementation exposes an ambiguity, update the specification, ADR, vectors,
both readers, and evidence together before declaring format freeze.

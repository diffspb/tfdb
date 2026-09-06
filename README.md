# TFDB — Telemetry Flash DB

Current development version: **1.1.0** (canonical value: [`VERSION`](VERSION)).
The latest immutable release tag remains **v1.0.0** until the 1.1.0 release
gates are completed.

TFDB (Telemetry Flash DB) is a small C++14 library for long-lived,
flash-conscious telemetry and log storage on Linux. A store is a fixed-size
file or block device split into a ring of independently recoverable partitions.
Data is appended in bounded blocks, old partitions are rotated in place, and
reads use compact per-block time indexes.

The project is intentionally dependency-free at runtime. It includes:

- a synchronous block/record writer and reader;
- a background asynchronous writer wrapper;
- a POSIX file/block-device backend and a deterministic in-memory fault backend;
- inspection, verification, extraction, and load-generation tools;
- unit, recovery, integration, and workload tests;
- a versioned on-media format specification and independent Rust read-only
  implementation with a shared conformance corpus.

## Build and verify

The only C++ runtime requirement is Linux with a C++14 standard library. The
default build uses `make` and produces a static library.

```sh
make
make check
make sanitize
make coverage
make fuzz
make benchmark
```

Equivalent CMake and Meson builds are maintained and tested:

```sh
cmake -S . -B build/cmake -G Ninja
cmake --build build/cmake
ctest --test-dir build/cmake --output-on-failure

meson setup build/meson
meson compile -C build/meson
meson test -C build/meson --print-errorlogs
```

The independent, standard-library-only Rust reader and the cross-language
corpus are checked separately so Rust never becomes a C++ runtime dependency:

```sh
make rust-test
make conformance
```

Public headers are under `include/tfdb/`. The core writer is synchronous and
single-writer; `AsyncWriter` is an optional bounded-queue wrapper. Both layers
offer checked append/submit operations that carry the expected record profile
and time domain across automatic rotation. Multiple readers may query candidate
blocks or use a record profile for exact time and selector filtering.

The tools are:

- `tfdb_format`: create/format a fixed regular file, or format an already
  provisioned device, which additionally requires `--yes` and refuses a path
  listed in `/proc/self/mounts`;
- `tfdb_inspect`: print geometry, generations, options, and time bounds;
- `tfdb_verify`: validate every currently reachable block and report gaps;
- `tfdb_dump`: stream candidate blocks or decoded FramedRecordV1 records;
- `tfdb_loadgen`: generate deterministic telemetry/log profiles, inject bounded
  short backend writes, verify exact retention, and report metrics.

## Design documents

A committed offline HTML mirror is stored under `docs/html/`; open
`docs/html/index.html` directly in a browser. Regenerate it with
`python3 docs/build_html.py` and verify that it matches the canonical Markdown
with `python3 docs/build_html.py --check`. Regeneration requires
exactly `markdown-it-py 3.0.0`; the generated pages have no runtime or network
dependency.

- [`CHANGELOG.md`](CHANGELOG.md): source/API release history and the explicit
  distinction from the persistent-format lifecycle;
- [`docs/roadmap.md`](docs/roadmap.md): post-1.0 priorities, format-freeze
  gates, native-Linux/hardware work, and the portable project handoff;
- [`docs/architecture.md`](docs/architecture.md): layers, lifecycle, durability,
  recovery, concurrency, and extension seams;
- [`docs/tutorial.md`](docs/tutorial.md): runnable programmer onboarding from
  provisioning through queries, async ingestion, logs, and deployment;
- [`docs/api.md`](docs/api.md): complete public C++14 API contracts, ownership,
  errors, metrics, and extension interfaces;
- [`docs/rust-reader.md`](docs/rust-reader.md): independent Rust API, tools,
  recovery behavior, and conformance workflow;
- [`docs/format-v1.md`](docs/format-v1.md): exact candidate wire format and
  compatibility rules;
- [`docs/decisions.md`](docs/decisions.md): alternatives and their effects;
- [`docs/research.md`](docs/research.md): surveyed systems and borrowed ideas;
- [`docs/testing.md`](docs/testing.md): acceptance and hardware qualification;
- [`docs/sizing.md`](docs/sizing.md): retention, loss, RAM, and eight-year
  endurance formulas;
- [`docs/evidence.md`](docs/evidence.md): current tests, coverage, exact I/O,
  and reference workload measurements;
- [`docs/logs.md`](docs/logs.md): application-log profiles, clock repair, and
  retention/security tradeoffs.
- [`docs/time-reconstruction.md`](docs/time-reconstruction.md): a non-mutating,
  uncertainty-aware proposal for 1970-era source timestamps.

Runnable, status-checked consumer examples are under `examples/`:

```sh
make -C examples check
```

See [`examples/quickstart.cpp`](examples/quickstart.cpp) for the synchronous
lifecycle and [`examples/async_writer.cpp`](examples/async_writer.cpp) for
bounded background ingestion. Production code must check every returned
`Status`. Destruction alone does not make a RAM tail durable.

## AI-agent integration skill

The repository ships a reusable skill at
[`skills/tfdb-integration/SKILL.md`](skills/tfdb-integration/SKILL.md). Invoke
it as `$tfdb-integration` in compatible coding agents when embedding TFDB or
reviewing an integration. It directs the agent to the current public headers,
safe provisioning, checked profile/time contracts, durability barriers,
backpressure and gap handling, and target-hardware qualification boundaries.

## Status

The C++14 candidate and an independent Rust reader are implemented. Their
shared feature/bounds, stale-writer, rotation, and corruption/recovery cases
agree, but format v1 is not frozen. Native-Linux model/fuzz/TSan/soak work,
representative replay, target power-cut/endurance qualification, and a real
pilot still have to pass. Continue with the ordered plan in
[`docs/roadmap.md`](docs/roadmap.md).

## License

TFDB is distributed under the [BSD 2-Clause License](LICENSE).

Copyright (c) 2026 Alexander Safronenko. Redistributions must retain the
copyright notice, license conditions, and disclaimer as specified by the
license, including in documentation or other materials accompanying binary
distributions.

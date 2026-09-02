# TFDB — Telemetry Flash DB

Current release: **1.0.0** (canonical value: [`VERSION`](VERSION)).

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
- a versioned on-media format specification intended for an independent Rust
  implementation.

## Build and verify

The only runtime requirement is Linux with a C++14 standard library. The
default build uses `make` and produces a static library.

```sh
make
make check
make sanitize
make coverage
make benchmark
```

Public headers are under `include/tfdb/`. The core writer is synchronous and
single-writer; `AsyncWriter` is an optional bounded-queue wrapper. Both layers
offer checked append/submit operations that carry the expected record profile
and time domain across automatic rotation. Multiple readers may query candidate
blocks or use a record profile for exact time and selector filtering.

The tools are:

- `tfdb_format`: create/format a fixed regular file or format an opened device;
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

- [`docs/architecture.md`](docs/architecture.md): layers, lifecycle, durability,
  recovery, concurrency, and extension seams;
- [`docs/tutorial.md`](docs/tutorial.md): runnable programmer onboarding from
  provisioning through queries, async ingestion, logs, and deployment;
- [`docs/api.md`](docs/api.md): complete public C++14 API contracts, ownership,
  errors, metrics, and extension interfaces;
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

The C++14 candidate is implemented and its current evidence is reproducible,
but format v1 is not frozen. It remains a candidate until an independent Rust
reader passes the golden/corruption vectors and target hardware passes physical
power-cut and eight-year endurance qualification.

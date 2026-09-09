# Changelog

This file records source/API releases. The persistent-format lifecycle is
separate: format v1 remains a candidate until the qualification gates in
[`docs/roadmap.md`](docs/roadmap.md) pass.

## Unreleased (target 1.1.0)

### Added

- independent dependency-free Rust reader, read-only tools, and shared
  C++/Rust conformance corpus;
- CMake and Meson build, install, package, and external-consumer support;
- public compile-time/runtime library version API;
- nonblocking writer-health snapshot for watchdogs;
- reproducible sanitizer-backed media fuzzer with process sharding;
- reproducible shared feature/bounds, stale-writer, and live-rotation corpus
  images with SHA-256 protection;
- `lz4_block:1`, the LZ4 raw block format, in both the C++ writer/reader and
  the Rust reader, with a normative byte grammar, a shared corpus image, and a
  codec stage in the media fuzzer; the reserved compression ID 2 is now
  assigned rather than reserved;
- BSD-2-Clause license and explicit project attribution.

### Changed

- split the C++ test suite into thematic scenario files;
- made ignored `Status` results diagnosable under C++17 with `[[nodiscard]]`;
- required `--device --yes` for block-device formatting and added a
  best-effort mounted-path refusal;
- documented reader freshness, early-boot entropy, async ownership, and
  durability contracts;
- deprecated `emergency_checkpoint()` and the ignored query CRC option;
- replaced checksum-time structure/record copies with incremental CRC32C.

### Fixed

- made the multi-reader hot-rotation regression deterministic by holding all
  four readers across actual slot reuse, removing scheduler-dependent empty
  runs under CPU saturation;
- converted exceptions escaping the asynchronous writer thread into a stored
  background failure instead of terminating the process;
- aligned Rust feature-region validation and damaged-header recovery with the
  documented C++ contract;
- published related writer-health fields with release/acquire ordering.

No persistent-format bytes changed in these post-1.0 changes. Format v1 is not
frozen.

## 1.0.0 - 2026-09-02

Initial immutable C++14 source release: synchronous ring store, bounded async
writer, file/block-device and deterministic fault backends, raw and PackBits
blocks, time index, recovery/gap handling, tools, tests, and documentation.

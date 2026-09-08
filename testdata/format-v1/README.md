# TFDB v1 shared conformance corpus

`valid-mixed.tfdb` is a deterministic 139264-byte volume written by the C++14
implementation through its public API. It contains one sealed PackBits
partition and one active uncompressed partition, two time domains,
out-of-order timestamps, an unsynchronized timestamp, an anomaly flag, and
eight valid FramedRecordV1 records.

`tests/make_conformance_volume.cpp` is its reproducible writer. The additional
committed images cover unknown optional and required features, invalid feature
and volume bounds, a stale writer-incarnation suffix, three consecutive
live-rotation snapshots, and an `lz4_block:1` partition.
`tests/make_conformance_cases.cpp` regenerates those images from either the
base volume, deterministic `MemoryStorage` crash and rotation schedules, or a
codec-specific write schedule.

`codec-lz4-block.tfdb` is an `lz4_block:1` partition whose four blocks cover
the decoder paths a reader has to get right: an extended literal run, extended
and short match lengths, an overlapping distance-one run, and a payload the
codec could not shrink, which therefore stores itself as `none` inside a
compressed partition. `codec-unknown-version.tfdb` is that same volume with
the compression feature claiming `lz4_block:2`; both readers must call it
unsupported at open rather than assume version 1 is close enough.

`SHA256SUMS` protects all committed images. The cross-language test regenerates
them byte-for-byte, compares C++ and Rust block output for every valid image,
and requires matching contract-level classifications for every manifest case.
The eight small legacy corruption/recovery mutations remain derived in the
test script rather than duplicated as committed files. Exact diagnostic
wording is not ABI.

The corpus is candidate-format evidence, not permission to silently change
format v1. Any encoded-byte change requires a new corpus, golden vectors, ADR,
and independent-reader review.

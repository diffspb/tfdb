# TFDB v1 shared conformance corpus

`valid-mixed.tfdb` is a deterministic 139264-byte volume written by the C++14
implementation through its public API. It contains one sealed PackBits
partition and one active uncompressed partition, two time domains,
out-of-order timestamps, an unsynchronized timestamp, an anomaly flag, and
eight valid FramedRecordV1 records.

The expected SHA-256 is
`7d81004d396903b4f7194a5635f31e11fbf4664ca2fa23b38a5e5588e5bac254`.
`tests/make_conformance_volume.cpp` is the reproducible writer. The
cross-language test regenerates the file byte-for-byte, compares C++ and Rust
inspection/block/record output, and verifies both implementations. It then
derives the recovery/corruption cases listed in `manifest.tsv` and requires
matching contract-level classifications; exact diagnostic wording is not ABI.

The corpus is candidate-format evidence, not permission to silently change
format v1. Any encoded-byte change requires a new corpus, golden vectors, ADR,
and independent-reader review.

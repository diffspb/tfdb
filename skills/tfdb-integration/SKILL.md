---
name: tfdb-integration
description: Integrate TFDB (Telemetry Flash DB) into Linux C++14 applications when adding telemetry or log persistence, record queries, bounded asynchronous ingestion, or a custom backend/profile. Use for implementation and integration review; do not use it to invent on-media format changes or claim unverified hardware guarantees.
---

# Integrate TFDB

Build a compiling, failure-aware integration against the public headers under
`include/tfdb/`. Preserve the application's requirements and TFDB's physical
order, bounded ring, and explicit durability model.

## Establish the contract

Inspect the consuming application, its build system, and the actual TFDB
headers/version before editing. In a TFDB source checkout, use these canonical
documents:

- `docs/tutorial.md` for lifecycle and working examples;
- `docs/api.md` for the current source contract;
- `docs/sizing.md` for retention, RAM, loss-window, and endurance calculations;
- `docs/logs.md` when application logs are in scope;
- `docs/architecture.md` and `docs/format-v1.md` only for architectural review
  or cross-language/media compatibility work.

Do not include `src/` headers or reproduce API signatures from memory. If the
installed headers disagree with the documentation, stop and report the version
mismatch rather than guessing.

Determine or make explicit assumptions for:

- regular file versus block device and whether provisioning is in scope;
- native self-framing records versus `FramedRecordV1`;
- selector mapping and record/profile versioning;
- application-provided index clock and time-domain ID;
- synchronous writer versus bounded `AsyncWriter`;
- maximum record/rate/burst, retention, power-loss budget, and media geometry;
- checkpoint/backpressure/gap/error policies;
- whether logs share telemetry retention and access policy.

Read [references/integration-checklist.md](references/integration-checklist.md)
when implementing or reviewing code. Use only the relevant sections.

## Implement safely

Prefer the smallest integration that meets the requirements:

- start with `FileStorage`, `FramedRecordV1`, and synchronous `RingStore` when
  the application has no established storage/profile abstraction;
- separate explicit provisioning from normal open/recovery;
- default file creation to `overwrite == false`;
- never format, resize, or overwrite an existing path or block device unless
  the user explicitly authorized that exact target and destructive operation;
- choose `OpenOptions::next_partition` before writable open;
- use `append_framed_record()` for synchronous FramedRecordV1, otherwise use
  `append_checked()` or `submit_checked()` for a nonzero profile;
- preserve source timestamps in payloads when unreliable and use the agreed
  application/arrival clock for the index;
- mark unsynchronized time and configure anomaly thresholds without dropping
  anomalous records;
- check every `Status`, including `checkpoint()`, `AsyncWriter::stop()`, and
  `RingStore::close()`.

Treat these as invariants:

- `publish()` is visibility, not durability; a successful checkpoint,
  graceful close, or completed rotation makes preceding data durable;
- `RingStore` destruction does not checkpoint, and `AsyncWriter` destruction
  discards the result of its implicit stop;
- one integration owner controls store mutations; do not mutate `RingStore`
  directly while its `AsyncWriter` is active;
- `StatusCode::busy` means an async record was not accepted; apply an explicit
  bounded retry, upstream backpressure, or accounted drop policy;
- surface background errors and recreate/reopen the writer stack after a
  fault; use `writer_status()` when checking store mutation readiness;
- query ranges are half-open, output is in physical rather than timestamp
  order, and gap events determine whether an export is complete;
- `query_records()` accepts one profile version; mixed-profile history requires
  `query_blocks()` plus application dispatch from `BlockMetadata`;
- callback byte views are borrowed and must be copied inside the callback;
- a checkpoint timer alone does not prove the loss bound: include queue delay,
  block publication, flush latency, and measured target behavior;
- software tests do not substitute for target eMMC/NAND power-cut and
  endurance qualification.

Do not invent profile IDs, selector layouts, time-domain meanings, application
flags, codecs, or on-media fields. If the application has not defined one,
propose a versioned project contract and make the choice visible for review.

## Validate the result

Produce a complete C++14 consumer, not an unbuildable fragment. Compile it
using only public includes and the static library, with warnings enabled and
`-pthread`. Run the narrow integration first, then the repository's relevant
checks.

For every generated integration, exercise:

- provision once, open without reformat, append, checkpoint, close, and reopen;
- exact half-open time/selector results with mixed timestamps;
- callback view copying and the selected time-quality policy;
- orderly shutdown and the error/backpressure path directly used by the task.

Select additional tests in proportion to the integration: rotation/profile
transition when configuration changes, async saturation and background errors
when async is used, reader gaps for export code, and injected write/flush
failures for writer fault handling. The full release qualification matrix in
the checklist is not mandatory for every narrow code-generation task, but
uncovered applicable items must be reported as follow-up work.

For a block-device deployment, provide a reviewed operational plan that checks
device identity, size, exclusivity, mount/use state, and recovery. Do not run a
destructive device command merely to validate generated code.

Report assumptions, files changed, commands run, observed results, and the
remaining hardware qualification boundary. Never describe an unmeasured loss,
retention, or lifetime target as guaranteed.

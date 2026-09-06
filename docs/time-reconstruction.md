# Reconstructing 1970-era timestamps (design proposal)

This document proposes an application/export-layer solution. It deliberately
does not implement clock correction in TFDB and does not change media format
v1. TFDB continues to preserve application-provided index time, source bytes,
physical order, time-quality flags, and gaps without rewriting evidence.

## Conclusion first

Exact wall time cannot always be recovered. If both a source and the storage
machine started with a 1970-era realtime clock, and records contain no trusted
wall-time anchor, boot identity, monotonic/uptime coordinate, sequence, or
external event correlation, many real timelines produce exactly the same
stored bytes. No algorithm can distinguish them.

Useful reconstruction is possible when each boot/session eventually has at
least one trusted synchronization anchor and records carry a coordinate that
survives the realtime correction. The recommended coordinate is monotonic
nanoseconds plus a random boot/session ID. A source sequence counter can give
order but normally cannot give elapsed time by itself.

Accordingly, reconstruction output must be an estimate with provenance and
uncertainty, never a silent replacement for the original timestamp.

## Keep four notions of time separate

| Value | Owner | Purpose |
|---|---|---|
| source realtime | MCU/Linux service | original event claim; preserved in payload |
| source monotonic/uptime | same source boot | stable event coordinate across realtime synchronization |
| storage arrival realtime | ingestion service | normal TFDB index when locally trustworthy |
| storage arrival monotonic | storage-machine boot | ordering and reconstruction when its realtime is wrong |

Also retain independent boot/session IDs for the source and storage machine.
Hot/cold redundant half-sets are separate sources even when they emit the same
message ID. Do not fit one clock model across a reboot, failover, counter reset,
or unknown gap.

Format v1 stores one application-selected index timestamp and opaque payload.
For new projects, use storage arrival realtime as the index when it is the most
reliable query clock, but place the other fields in the project record schema.
When storage realtime is also unsynchronized, retain the record, set the
unsynchronized flag, and make physical scan available to exporters.

## Minimum project envelope

For data that may require clock repair, the payload or its registered project
profile should contain:

- stable logical source ID, physical half-set/address, and service ID;
- random 128-bit source boot/session ID;
- source realtime in signed nanoseconds exactly as produced;
- source monotonic/uptime counter, its frequency, width, and wrap rules;
- monotonically increasing source sequence where available;
- time-quality state (`unset`, `acquiring`, `synchronized`, `holdover`,
  `known_bad`) and synchronization-event marker;
- storage boot/session ID and storage-arrival monotonic time, added by the
  ingestion service;
- storage-arrival realtime and its quality state;
- for an anchor, the sampled realtime/monotonic pair and stated uncertainty.

Do not use an all-zero boot ID as a normal value. If an MCU cannot create a
random ID, combine a persistent boot counter with device identity and mark the
weaker collision guarantee explicitly.

A time-validity rule such as “year is at least 2020” is project configuration,
not part of TFDB. It must account for legitimate historical simulations and
future lifetime; the raw numeric value alone is not proof of synchronization.

## Proposed reconstruction pipeline

### 1. Extract immutable evidence

Read in physical order and retain a stable locator for every record:

```text
(volume_id, partition_generation, slot, block_sequence, record_position)
```

Keep the original encoded record and all timestamps. A corrupt/overwrite gap
terminates continuity until a new independently identified session or anchor;
the algorithm must not interpolate silently across it.

### 2. Segment clocks

Group by source identity and source boot/session ID. Inside a group, split again
on monotonic reset/wrap ambiguity, sequence reset, explicit reboot/failover,
impossible rate, or discontinuity larger than the project's bound. Segment the
storage-arrival clock independently by storage boot ID.

Physical order resolves slightly interleaved arrival streams but does not mean
source timestamps are sorted. Redundant devices must never share a segment
solely because their logical messages match.

### 3. Select trusted anchors

An anchor is a measurement connecting stable monotonic coordinate `M` to wall
time `W`, with uncertainty `U`. Prefer explicit synchronization telemetry
produced after the clock service declares lock. Cross-check it against storage
arrival and known external events. Reject isolated anchors that violate
monotonic order, allowed slew/rate, session identity, or transport-delay
bounds.

One anchor determines an offset but not clock-rate error. Two or more separated
anchors allow a bounded affine model:

```text
W(M) = W0 + slope * (M - M0)
```

Fit per segment, constrain `slope` by the oscillator/synchronization policy,
and use a robust estimator so one bad synchronization sample cannot shift a
whole boot. Because the deployed systems slew rather than step time backward,
the accepted mapping should be nondecreasing. A forward synchronization step
starts a new piece or a documented transition interval; do not smear an
instantaneous step backward over earlier records.

### 4. Estimate and bound

For records between anchors, interpolate with the local model. For pre-sync
records, extrapolate backward from the first trusted anchor using source
monotonic time. Uncertainty should grow at least as:

```text
anchor uncertainty
+ oscillator drift bound × distance from anchor
+ timestamp sampling error
+ bounded transport/queue uncertainty when arrival time is used
```

If only ordering is known, emit an ordered interval or `order_only`, not a
fabricated nanosecond. If source monotonic is absent but storage arrival
monotonic and bounded delivery latency are present, provide a wider arrival-
based interval. If no defensible bound exists, leave reconstructed time empty.

### 5. Publish a sidecar view

Never mutate the TFDB volume or original message. The exporter should emit:

```text
original_source_time_ns
reconstructed_time_ns (optional)
lower_bound_ns / upper_bound_ns (optional)
method
quality
anchor IDs and model revision
physical record locator
```

Suggested quality classes are:

- `observed_trusted`: original wall clock was synchronized;
- `anchored`: reconstructed between trusted anchors;
- `extrapolated`: reconstructed before/after anchors with a stated bound;
- `arrival_bounded`: only a delivery/arrival interval is defensible;
- `order_only`: sequence is known but wall time is not;
- `unrecoverable`: insufficient evidence or conflicting anchors.

Corrections can be stored in an export manifest or analytical database keyed
by the physical locator. Re-running with better anchors creates a new model
revision; it does not rewrite history.

## Use with the current TFDB index

The TFDB block index accelerates queries in one declared time domain; it is not
the reconstructed clock model. A 1970-era interval may require
`scan_blocks()`/physical extraction because a normal wall-time range will not
select it. Once extracted, reconstruction and optional timestamp sorting occur
above TFDB.

Do not insert a reconstructed value back into the source field. If a project
needs indexed corrected views, build them as disposable derived data that can
be regenerated from the immutable TFDB volume and correction manifest.

The existing `time_anomaly` and `unsynchronized_time` aggregate flags are
useful discovery hints, not proofs about every record. Exact record quality
must live in the record profile.

## Important corner cases

- A source timestamp near 1970 may be valid test data; quality metadata wins
  over a hardcoded calendar threshold.
- Monotonic counters wrap. Unwrap only with documented width/frequency and a
  gap shorter than the ambiguity interval.
- A reboot may repeat uptime and sequence values. Boot ID is mandatory for a
  confident join.
- Clock synchronization can be wrong before becoming right. Require a stable
  lock interval or multiple consistent anchors.
- Queueing makes arrival time an upper bound only when the transport path and
  timestamp point are known; retransmission can widen or invalidate it.
- Slightly disordered producers are normal. Preserve physical order and fit
  each source separately.
- Hot standby duplicates must retain their physical source identity; dedup is
  a separate policy.
- A forward wall-time step can create a range with no events. A slew changes
  slope. Model these explicitly.
- Corruption/overwrite gaps may hide a reboot or adjustment event. Continuity
  after a gap requires a new anchor, not optimism.

## Qualification plan for a future implementation

Before shipping a reconstruction tool, build deterministic synthetic traces
with known truth for: both clocks bad at boot; source-only and storage-only bad
clocks; one/two/many anchors; oscillator drift; forward steps; slew; reboot;
counter wrap; redundant half-sets; transport bursts; missing anchor; corrupt
gap; and contradictory anchors.

Measure coverage/abstention as well as timestamp error. A correct tool must
prefer `order_only` or `unrecoverable` over an unjustified precise answer.
Acceptance should bound the worst reconstruction error for each quality class,
verify that reported intervals contain ground truth, and prove deterministic
results from the same evidence/model revision.

The recommended implementation is a separate offline/export library and CLI,
not code in `RingStore`. That keeps the storage core simple, avoids migrations,
allows project-specific clock models, and preserves the evidence needed to
improve a reconstruction later.

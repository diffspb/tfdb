# Architecture decision log

This log records important alternatives and their effects. Accepted decisions
may be revisited before format v1 is declared stable.

| ID | Decision | Alternatives rejected/deferred | Main effect |
|---|---|---|---|
| ADR-001 | Fixed volume split into a ring of large partitions | Growing files; general DB/LSM | Deterministic capacity and rotation; overwrite is explicit |
| ADR-002 | Per-partition generation; no frequently updated global head | Mutable superblock head pointer | Low metadata wear; startup scans all small partition headers |
| ADR-003 | Variable-length encoded blocks written sequentially | Fixed physical slots | Compression saves capacity and host bytes; recovery must validate lengths |
| ADR-004 | CRC32C on every structural header and block payload | Generation only; optional checksum | Detects torn writes and latent corruption with negligible media overhead |
| ADR-005 | Footer index is rebuildable acceleration data | Index updated on every block | Sequential data writes and low amplification; active startup scans one partition |
| ADR-006 | Core accepts whole opaque records plus an index timestamp | Core owns all project schemas; prebuilt blocks only | Core can assemble safe blocks without fixing telemetry representation |
| ADR-007 | Block query below record parsing/filtering | Message IDs embedded in the volume format | Keeps storage generic; project codec is required for message-level tools |
| ADR-008 | Per-block min/max time, physical result order | Globally sorted timestamp tree | No false negatives with modest disorder; false-positive block reads are acceptable |
| ADR-009 | Raw time is never corrected; anomaly is an aggregate block flag | Rewrite time; discard bad records | Preserves evidence and keeps schema-independent core; exact bad record needs upper codec |
| ADR-010 | Readers never pin rotation | Reference-counted partition leases | Writer cannot be stalled; reader receives an explicit overwritten gap |
| ADR-011 | Checkpoint closes the partial RAM block before `fdatasync` | Sync only already-written full blocks | A configured loss bound also holds for sparse traffic, at some block overhead |
| ADR-012 | Synchronous core plus optional background wrapper | Hidden thread in every writer; Linux AIO | Simple deterministic core and portable C++14 API; async wrapper owns queue/backpressure |
| ADR-013 | Unknown optional features are skipped; unknown required features fail | Interpret unknown data heuristically | Safe forward compatibility and independent Rust implementation |
| ADR-014 | Normal open uses valid prefix; verify/salvage are separate | Automatic scan past interior corruption | Predictable operation without silently mixing questionable data |
| ADR-015 | File and block device share the positional-I/O backend | Separate formats/backends | Same recovery semantics; hardware durability remains a deployment property |
| ADR-016 | Random 128-bit format-incarnation ID is separate from project identity | Clock/PID-derived or stable volume ID | Prevents stale data resurrection after reformat; formatting requires `getrandom()` |
| ADR-017 | CRC32C is mandatory on live reads; generation is rechecked after index acquisition, payload read, and apparent validation failure | Optional query checksum; classifying CRC failure without a generation recheck | Reader/rotation races become explicit overwrite gaps, while unchanged-generation damage remains corruption |
| ADR-018 | Block starts are separated by a power-of-two persistence quantum | Dense byte packing; fixed block slots | Simple recovery/isolation with bounded alignment waste; quantum is not a physical atomicity claim |
| ADR-019 | Default open is lazy; strict open eagerly validates sealed payloads, footer summaries, and every recovered footerless prefix | One fixed integrity policy | Fast normal startup and an explicit higher-assurance service level share one format |
| ADR-020 | Buffered positional I/O plus explicit `fdatasync` in v1 | `O_DIRECT`; `O_DSYNC` on every write | No platform-specific buffer alignment, checkpoints remain configurable; page-cache effects must be measured |
| ADR-021 | Advisory exclusive writer lock; readers never lock out rotation | Uncoordinated writers; reader leases | Prevents ordinary multi-process writer corruption while preserving live inspection |
| ADR-022 | Full fixed-size v1.0 structures and rejection of future minor versions | Guessing prefix-compatible structures | Conservative independent implementations; future additive rules require a specification update |
| ADR-023 | Implement optional FramedRecordV1 outside block format | Mandatory common record envelope | Exact selector/time queries are available without taxing self-framing project protocols |
| ADR-024 | Keep telemetry/log retention isolation as a deployment decision | Implicit priority inside one ring | Same storage code supports logs; separate volumes prevent log storms evicting critical telemetry |
| ADR-025 | Chain adjacent blocks with random per-open 128-bit writer IDs | Resume using generation/sequence alone; seal a large partition on every boot | Prevents stale-suffix resurrection across two crashes while preserving partition capacity; adds 32 bytes per block |
| ADR-026 | Typed append atomically checks record profile and time domain after auto-rotation | Check then call raw append | A configuration boundary cannot mislabel a typed record; rejection may still publish/rotate the preceding full block |
| ADR-027 | Domain zero is physical-scan only, never a numeric range wildcard | Compare one interval across unrelated clocks | No accidental comparison of incomparable clocks; callers use `scan_blocks()` |
| ADR-028 | Writable open rejects a damaged header with current-volume block/footer evidence | Guess generation or silently overwrite the damaged slot | Prevents generation collision and stale-data resurrection; lazy read-only inspection remains available |
| ADR-029 | Async deadline and age metrics use one monotonic clock, with an injectable clock/wakeup seam | Real sleeps in deterministic tests; separate timer and metric clocks | Exact loss-bound tests include backend stalls without changing the media format or production thread model |
| ADR-030 | Async timer uses elapsed unsigned time from the last checkpoint, bounded wait chunks, and rejects millisecond intervals that cannot fit in `uint64_t` nanoseconds | Saturating absolute deadlines; signed `duration_cast`; one enormous `wait_for` | No conversion/library-internal overflow or repeated checkpoint loop when an injected clock reaches `UINT64_MAX` |
| ADR-031 | Load qualification uses a bounded persisted-write oracle and capped deterministic latency reservoirs | Retain every generated payload; validate only whatever a query happens to return | Exact retention-window checks and stable memory use remain practical for long rotation workloads |

## Open decisions before format freeze

- Whether format v1 ships the simple PackBits codec only or also a reviewed
  dependency-free LZ4 block implementation.
- The block-summary callback and media allocation contract for future
  project-specific value indexes.
- Whether the proposed dual-clock logging profile belongs in this repository
  or a companion schema package.
- Whether very large sealed indexes need a paged reader before format freeze;
  this does not require a media change but affects peak RAM.

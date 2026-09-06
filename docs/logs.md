# Application logs in TFDB

TFDB's block layer can store logs without a separate persistence mechanism.
That is attractive because log bursts, crash recovery, time problems, and
rotation are already handled. It does not mean telemetry and logs should
always share one volume.

## Integration levels

### Existing binary log record

If a project already has a length-delimited binary log event containing its
timestamp and component ID, pass one complete event to `RingStore::append()`
and register a project record-profile ID. This has the least overhead and
retains the original evidence.

### FramedRecordV1

For plain text or arbitrary blobs, `FramedRecordV1` supplies framing, whole-
record CRC, application timestamp, 64-bit selector, and flags. Use the selector
as a stable component/channel ID. Severity and structured fields remain in the
payload. This mode is implemented and used by the `logs` load-generator
profile.

### Future common LogRecord profile

A cross-project log standard should be a record profile, not fields in every
TFDB block. A candidate profile needs at least:

| Field | Purpose |
|---|---|
| total length and profile version | bounded skip and evolution |
| realtime nanoseconds | operator-facing time and normal range query |
| monotonic nanoseconds | order within one boot when realtime is wrong |
| 128-bit boot/session ID | prevents comparing monotonic values across boots |
| 64-bit component/channel ID | stable filter key |
| severity and quality flags | diagnostics and optional future index |
| payload encoding ID | UTF-8 text, CBOR/project binary, or template arguments |
| optional template/event ID | compact repeated messages |
| payload and whole-record CRC | exact preservation and salvage |

This envelope is intentionally not frozen in v1. For short telemetry it would
be excessive; typical log messages are large enough that its overhead is less
important. A future version needs independent C++/Rust vectors before getting
a persistent profile ID.

## Clock handling

Logs written before clock synchronization must not be discarded or rewritten.
The complete proposal, including cases where recovery is impossible, is in
[`time-reconstruction.md`](time-reconstruction.md). In summary, an exporter:

1. group records by boot/session ID;
2. preserve physical record order as the primary evidence;
3. find clock-synchronization anchors containing both realtime and monotonic
   values;
4. estimate wall time on each side of an anchor using monotonic deltas;
5. label reconstructed values as estimates and retain the original timestamp;
6. never compare monotonic values belonging to different boot IDs.

The common block index remains based on the application-selected primary time.
Blocks with unsynchronized or anomalous time carry flags, so tools can include
or highlight questionable intervals. A physical scan is always available when
a wall-time query cannot describe the desired period.

## Retention and overload

Logs and telemetry should share a volume only if all of these policies match:

- retention horizon;
- acceptable loss interval;
- privacy/access controls;
- peak-rate budget;
- overwrite priority.

A debug log storm must not evict safety telemetry unexpectedly. Separate fixed
volumes are the simplest isolation. If they share one volume, the application
must enforce admission control before TFDB; the synchronous core intentionally
has one writer and no priority scheduler. The async wrapper reports queue
backpressure and accepted-to-durable latency.

Text and template logs often compress well with dictionary/match codecs. The
dependency-free v1 PackBits codec only captures adjacent equal-byte runs and
barely changed the reference log workload, so it must not be used to predict a
general text-compression gain. Binary attachments and already compressed
payloads usually do not compress; TFDB's per-block raw fallback avoids
expansion. Large attachments should normally live in a separate record/profile
or store so a single item cannot dominate block and retention sizing.

## Security and privacy

CRC32C detects accidental corruption; it provides neither confidentiality nor
authenticity. Logs may contain credentials or personal data. Encryption,
redaction, key rotation, authenticated export, and secure erase are deployment
requirements outside media format v1. A circular overwrite is not proof that
old NAND cells are physically unrecoverable behind an FTL.

## Qualification profile

The `logs` workload emits variable-size repeated text with component selectors.
A project log qualification should additionally include multiline data, NUL
and invalid UTF-8 bytes, maximum-size records, severity bursts, unique random
messages, and simultaneous telemetry load. Report retention, compression,
host-write amplification, queue residence, accepted-to-durable latency, query
latency, and CPU use rather than relying only on average throughput.

# TFDB integration checklist

Use this reference while changing or reviewing a consuming application. The
installed public headers and the application's project contracts remain the
source of truth.

## Decision record

Record these choices in application documentation or configuration review:

| Decision | Required evidence |
|---|---|
| Backend and target | regular file/block device path ownership; fixed size; flush contract |
| Provisioning | explicit trigger; overwrite authorization; geometry source |
| Record contract | native/FramedRecordV1; stable ID/version; maximum encoded record |
| Selector contract | stable bit/registry mapping for source/service/message or log class |
| Index time | producing application clock, time-domain ID, unsynchronized policy |
| Writer | synchronous owner or AsyncWriter queue capacities and producer policy |
| Durability | checkpoint policy, maximum queue residence, worst flush latency, loss budget |
| Retention/endurance | measured input profiles, rotation rate, host writes, media margin |
| Reader completeness | half-open range, selector semantics, physical order, gap handling |
| Shutdown/recovery | stop/checkpoint/close order and response to a faulted writer |

Unknown project IDs or destructive targets require user review. Ordinary code
structure, error plumbing, and safe `overwrite == false` examples do not.

## Safe lifecycle patterns

Provisioning:

1. Resolve and validate the exact target.
2. For a new regular file, call `FileStorage::create(..., false, ...)`.
3. For an authorized existing block device, use `open_existing(..., true, ...)`.
4. Calculate and validate `VolumeOptions`.
5. Call and check `RingStore::format()` once.

Normal startup:

1. `FileStorage::open_existing()` without create/format.
2. Fill `OpenOptions`, including historical codecs and the next partition
   contract.
3. `RingStore::open()` and retain the storage `shared_ptr`.
4. Check `writer_status()` before attaching an asynchronous writer.

Synchronous shutdown:

1. Stop upstream submission.
2. Check `RingStore::close()`.
3. Release store, then backend.

Asynchronous shutdown:

1. Stop upstream producers.
2. Check `AsyncWriter::checkpoint()` if the service needs an explicit barrier.
3. Check `AsyncWriter::stop()`; destroy the wrapper before the store.
4. Check `RingStore::close()`.
5. Release store, then backend.

## Record mapping review

- Native records must be complete, nonempty, concatenable, and recoverably
  self-framing inside a decompressed block.
- FramedRecordV1 adds 32 bytes per record; include that in the maximum encoded
  record and block-packing calculations.
- A profile decoder validates every bound/checksum, emits records in physical
  order, honors visitor false, and emits the declared count.
- Use checked append/submit so an automatic rotation cannot silently change
  profile or time domain.
- A source device timestamp belongs in the payload when it can be wrong. The
  storage-service arrival clock is usually the robust index clock.
- Preserve stable sender/half-set/service/message identity in selector or
  payload; never use transient process IDs as a persistent selector contract.

For application logs, include severity and structured attributes in the
versioned payload. Use a separate volume when retention, overload priority,
access control, or failure isolation differs materially from telemetry.

## Status and state review

Switch on `StatusCode`, not message text.

| Result | Integration response |
|---|---|
| `ok` | continue |
| `busy` from async submit | record was not accepted; bounded retry/backpressure/accounted drop |
| `overwritten` gap | mark export incomplete because the reader lost rotation race |
| `corrupt` gap | mark export incomplete, retain physical diagnostics, continue only by policy |
| write/flush/background error | stop writer path, preserve original error, reopen/recover before resume |
| `closed` | reject new work or fix lifecycle ordering |
| `unsupported` | register historical codec/profile or reject incompatible media |
| `generation_exhausted` | stop writes; preserve read access and escalate replacement/reformat plan |

Default `continue_on_gap == true` can return overall OK after delivering gap
events. Completeness must therefore be tracked separately from the final
status.

## Code review rejection patterns

Reject an integration that:

- includes `src/internal_format.hpp` or depends on internal constants;
- formats during every startup or defaults to overwrite true;
- ignores any returned `Status`;
- labels append/publish/destruction as durable;
- lets an AsyncWriter outlive its RingStore;
- mixes direct store mutation with an active async wrapper;
- loops indefinitely or spins on `busy`;
- retains callback `ByteView` objects without copying;
- assumes query output is timestamp-sorted;
- discards or hides gap events;
- uses deterministic volume/writer IDs in production;
- promises power-loss/endurance behavior without target-device evidence;
- changes on-media/profile semantics as an incidental application integration.

## Release/reference acceptance matrix

Apply the rows relevant to the changed integration. A narrow generated
consumer must always cover public consumption, safe creation, recovery, query,
and lifetimes; add the remaining rows when that feature or failure path is in
scope. The complete matrix is release evidence, not a prerequisite for every
small code-generation task.

| Area | Minimum observable check |
|---|---|
| Public consumption | C++14 build with only `include/tfdb`, static library, and pthread |
| Safe creation | existing path is rejected when overwrite is false |
| Recovery | checkpointed records survive crash/reopen; uncheckpointed tail may be absent |
| Query | exact selectors and `[begin,end)` timestamps; mixed input remains physical order |
| Lifetimes | consumer copies payload needed after callback |
| Rotation | retained set and expected overwritten gap are checked independently |
| Time quality | unsynchronized and anomaly block flags are observed without record loss |
| Async | full queue returns busy; barrier covers accepted prefix; stop drains |
| Fault | injected write/flush error reaches caller and no false durable watermark advances |
| Logs | binary/NUL payload and configured selector/time policy round-trip if logs are enabled |

Use `MemoryStorage` for deterministic failure/crash tests and `CountingStorage`
for host-I/O assertions. Keep physical power-cut, controller cache, throughput,
retention, and eight-year endurance qualification as separate release evidence.

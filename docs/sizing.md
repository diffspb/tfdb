# Sizing, loss bound, RAM, and eight-year endurance

No single default can prove retention or flash life for all projects. TFDB
exposes the measurements needed to size a project and keeps the formulas
explicit.

## 1. Definitions

| Symbol | Meaning |
|---|---|
| `P` | partition size |
| `I` | index reserve per partition |
| `B` | maximum raw block payload |
| `Q` | persistence quantum |
| `N` | complete partition count |
| `R` | measured framed input bytes/second at the core |
| `S` | stored payload bytes in one block after raw fallback |
| `F` | frame bytes actually written, `128 + S` |
| `A` | address span consumed, `ceil(F/Q) × Q` |
| `E` | serialized record-envelope bytes per second |
| `W_host` | successful bytes requested from the backend per second |
| `WA_fs` | filesystem allocation/journal amplification |
| `WA_ftl` | device/controller internal write amplification |

The data address capacity of one partition is:

```text
D = P - 4096 partition header - I - 4096 footer region
```

The index holds exactly `floor(I / 48)` block entries. The partition seals when
either the next worst-case frame reservation cannot fit or the next index entry
cannot fit. Data-capacity exhaustion sacrifices less than one worst-case aligned
frame. Index exhaustion can leave a larger data remainder; both losses are
deliberate consequences of fixed bounds.

## 2. Retention

Measure block fill and compression with representative input. For a sequence of
blocks, usable raw payload retention is:

```text
retained_raw_bytes = sum(raw_size for retained frames)
retention_seconds  = retained_raw_bytes / R
```

Because the simple writer reserves one worst-case frame before starting every
new block, an exact capacity calculation should replay the measured block-size
sequence through that policy. For constant actual span `A` and worst-case span
`A_worst`, a closer bound is:

```text
frames_by_data = 0,                                      if D < A_worst
                 1 + floor((D - A_worst) / A),           otherwise
frames_per_partition       = min(frames_by_data, floor(I / 48))
raw_per_partition          = frames_per_partition × average_raw_size
maximum_retention_seconds  = N × raw_per_partition / R
minimum_ring_phase_seconds = (N - 1) × raw_per_partition / R
```

`N × raw_per_partition` is reached just before reuse. Immediately after the
oldest slot becomes a nearly empty active partition, only approximately `N-1`
full partitions are guaranteed. Retention requirements must use the minimum
ring phase (or, preferably, the minimum from an exact replay across all phases),
not the pre-rotation maximum. This constant-span expression is illustrative;
use the measured distribution, not only an average, for sparse traffic and
bursts. Every sparse checkpoint can create a small frame that still consumes
one quantum of address space. Conversely, padding is not physically written,
so address-space efficiency and host-write amplification are separate metrics.
If the index fills first, an arbitrary remainder of the data region can remain
unused; the fixed reserve makes this explicit rather than risking overflow.

Example geometry from the earlier design discussion—64 MiB partition, 1 MiB
index, 32 KiB raw block, 4 KiB quantum—has about 62.99 MiB of data address
space. A full raw block consumes 36 KiB because its 128-byte header crosses the
next 4 KiB boundary, so approximately 1791 such frames fit and require only
about 84 KiB of index. The larger 1 MiB reserve is useful when frequent sparse
checkpoints create 4 KiB spans. This is an intentional space-for-simplicity
tradeoff, not hidden overhead.

## 3. Configured power-loss bound

For the synchronous API, the application owns scheduling. For the async
wrapper, a record is first accepted into a bounded queue. The bound to qualify
is:

```text
accepted_to_durable_max
  = checkpoint scheduling delay
  + queue residence
  + block publication time
  + successful flush time
```

Choose a target `XXX` seconds and require, with safety margin:

```text
metrics.maximum_accepted_to_durable_ns <= XXX × 1e9
```

under the worst qualified peak and sparse profiles. Also require zero sync
errors and no unbounded backpressure. A timer interval of `XXX` alone is not a
guarantee if producers can outrun the writer. On emergency power indication,
`checkpoint()` still needs enough hold-up time for the observed
worst-case publication plus flush; otherwise the normal periodic bound applies.

Application acknowledgment semantics must be explicit:

- accepted acknowledgment permits loss until the next checkpoint;
- durable acknowledgment waits for the checkpoint barrier;
- published is useful for live visibility but is not a power-loss guarantee.

## 4. Host writes and index overhead

In steady state each block writes `128 + stored_size` bytes. Padding to `Q` is not
written. A sealed partition additionally writes:

```text
48 × block_count + 256 footer + 1024 next partition header
```

Volume headers are written only on format. Flushes may cause additional media
traffic that byte counters cannot see.

The library reports:

- framed input bytes;
- stored/raw block bytes;
- backend write calls and transferred bytes;
- flush count and duration;
- partition rotations;
- query/recovery read bytes.

For a regular file, measure filesystem journal/allocation traffic separately
on the final filesystem and mount configuration. For a block device, include
partitioning/provisioning and controller behavior. Neither host count reveals
`WA_ftl`.

## 5. Eight-year endurance calculation

Eight Julian years are approximately 252,460,800 seconds. First calculate host
writes from the worst credible long-term workload:

```text
host_bytes_8y = measured_W_host × 252460800
```

Translate application writes to the interface at which the endurance limit is
specified. For a regular file, first estimate writes submitted to the device:

```text
device_interface_bytes = host_bytes_8y × WA_fs
required_TBW          = device_interface_bytes × qualification_margin
```

Vendor TBW/DWPD normally counts host writes at the device interface; applying
`WA_ftl` before comparing with such a limit double-counts the controller's
internal amplification. If qualification instead uses a NAND-write/P/E model,
then calculate:

```text
estimated_nand_bytes = device_interface_bytes × WA_ftl
required_nand_bytes  = estimated_nand_bytes × qualification_margin
```

and compare that result with a NAND-level capacity/cycle model. A rough model
when only program/erase cycles are available is:

```text
nominal_nand_endurance = usable_flash_bytes × rated_PE_cycles
```

but over-provisioning, static data, bad-block reserve, wear leveling, SLC cache,
write amplification, and required end-of-life retention can dominate. A host
ring does not guarantee even physical wear by itself because the FTL remaps it.

As an illustrative scale only, a continuous 2 MiB/s framed stream produces
about 529 TB in eight Julian years before filesystem/FTL amplification. That
number immediately excludes many consumer eMMC parts even if the logical ring
is only 128 GiB. Use the actual measured project rate and a part-specific
endurance commitment.

Qualification should record available device health counters throughout an
accelerated write test, but such counters are not a substitute for destructive
endurance and retention validation.

## 6. RAM bounds

Ignoring allocator bookkeeping, the synchronous worst case is approximately:

```text
decoded_index_entry_bytes = implementation sizeof(IndexEntry)
                           (64 bytes in the reference x86_64 C++ build;
                            48 bytes is only the serialized media entry)

writer <= B raw builder
        + codec max-compressed candidate
        + (128 + max(B, compressed_used)) contiguous frame
        + floor(I / 48) × decoded_index_entry_bytes

reader per concurrent query <= one stored block + one decoded block
                             + one serialized partition index (at most I)
                             + floor(I / 48) × decoded_index_entry_bytes
                             + at most one copied active-index vector of the
                               same decoded-entry bound

open recovery transient <= two footerless decoded-index vectors; older ones
                          are released as slots are scanned, so this is O(I),
                          not O(partition_count × I)

async addition <= configured queue byte capacity
                + bounded per-record queue metadata
                + one oldest-undurable timestamp
```

For TFDB PackBits the candidate bound is
`B + ceil(B/128)`. The `none` codec skips that candidate copy. Multiple readers
multiply reader memory; they do not pin media. Measure RSS with the final
allocator and concurrency because vector capacity and thread stacks are not in
the formulas.

## 7. Recovery and query I/O bounds

Normal open reads two 256-byte volume headers plus a 1024-byte header and
256-byte footer from each partition. Every current-volume partition without a
valid footer is scanned to its valid block prefix. A normal interrupted
rotation should leave one; independent footer damage can require more.

Lazy sealed queries read one index (at most `I`) and candidate frames. Strict
open additionally reads all sealed indexes and blocks. Exact record filtering
may read false-positive blocks selected by min/max time; selector filtering in
v1 does not have a secondary media index.

## 8. Project sizing procedure

1. Capture representative and worst-case streams before choosing geometry.
2. Include native framing, redundant senders, clock faults, service logs, and
   sparse idle periods.
3. Run all workload profiles with candidate `P/I/B/Q`, compression, and
   checkpoint interval.
4. Choose retention with margin after observing quantum/index waste.
5. Verify peak drain rate and accepted-to-durable maximum.
6. Calculate eight-year host bytes, then apply measured conservative physical
   amplification and endurance margin.
7. Repeat on the exact filesystem and raw-device configurations.
8. Validate with automated physical power cuts and accelerated wear/retention.

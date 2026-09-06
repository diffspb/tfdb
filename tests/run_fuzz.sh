#!/usr/bin/env bash
# Shards tfdb_fuzz_media across independent processes.
#
# Sharding by process rather than by thread is deliberate. Each shard is a
# separate seed stream over its own images, so nothing is shared and the
# (seed, iteration) pair still identifies an image exactly. Threads would add
# no throughput here and would make a failure ambiguous between a defect in the
# library and a race in the harness; this fuzzer's oracle is about media
# decoding, not concurrency. A sanitizer abort also stays contained in the
# shard that hit it, leaving the others to finish.
set -uo pipefail

binary=${FUZZ_BINARY:?FUZZ_BINARY is required}
corpus=${FUZZ_CORPUS:?FUZZ_CORPUS is required}
iterations=${FUZZ_ITERATIONS:-20000}
seed=${FUZZ_SEED:-20260906}
jobs=${FUZZ_JOBS:-1}
artifacts=${FUZZ_ARTIFACTS:-build/fuzz}
timeout_seconds=${FUZZ_TIMEOUT:-0}
report_every=${FUZZ_REPORT_EVERY:-}
# Any stride coprime with the generator's period does; a large odd number keeps
# shard seeds visibly distinct in logs and in artifact filenames.
stride=${FUZZ_SEED_STRIDE:-1000003}

# Validate before anything else: a non-numeric job count would otherwise make
# the shard loop run zero times and report a successful run that did no work.
for pair in "FUZZ_JOBS=$jobs" "FUZZ_ITERATIONS=$iterations" \
            "FUZZ_SEED=$seed" "FUZZ_SEED_STRIDE=$stride" \
            "FUZZ_TIMEOUT=$timeout_seconds"; do
  if ! [[ "${pair#*=}" =~ ^[0-9]+$ ]]; then
    echo "$pair is not a non-negative integer" >&2
    exit 2
  fi
done
if [ "$jobs" -lt 1 ]; then
  echo "FUZZ_JOBS must be at least 1" >&2
  exit 2
fi
if [ "$iterations" -lt "$jobs" ]; then
  echo "FUZZ_ITERATIONS ($iterations) must be at least FUZZ_JOBS ($jobs)" >&2
  exit 2
fi

mkdir -p "$artifacts"
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}

# FUZZ_ITERATIONS is the total across shards, so a job count change alters
# wall time rather than how much work is done. The remainder goes to shard 0.
per_shard=$((iterations / jobs))
remainder=$((iterations % jobs))

if [ -z "$report_every" ]; then
  report_every=$((per_shard / 4))
  [ "$report_every" -lt 1 ] && report_every=0
fi

pids=()
seeds=()
logs=()
counts=()
for ((shard = 0; shard < jobs; ++shard)); do
  shard_seed=$((seed + shard * stride))
  shard_iterations=$per_shard
  [ "$shard" -eq 0 ] && shard_iterations=$((per_shard + remainder))
  shard_log="$artifacts/fuzz-seed-$shard_seed.log"
  command=("$binary" --corpus "$corpus" --iterations "$shard_iterations"
           --seed "$shard_seed" --artifacts "$artifacts"
           --report-every "$report_every")
  if [ "$timeout_seconds" != "0" ]; then
    command=(timeout "$timeout_seconds" "${command[@]}")
  fi
  (
    set -o pipefail
    # sed -u: without it progress lines sit in sed's buffer whenever stdout is
    # a pipe or file, so a redirected or CI run shows nothing until a shard
    # finishes. The fuzzer already flushes its own progress lines.
    "${command[@]}" 2>&1 | sed -u "s/^/[shard $shard] /" | tee "$shard_log"
  ) &
  pids+=("$!")
  seeds+=("$shard_seed")
  logs+=("$shard_log")
  counts+=("$shard_iterations")
done

failed=0
for ((shard = 0; shard < jobs; ++shard)); do
  if ! wait "${pids[$shard]}"; then
    failed=1
    echo "shard $shard FAILED: seed=${seeds[$shard]} log=${logs[$shard]}" >&2
  fi
done

if [ "$failed" -ne 0 ]; then
  echo "tfdb_fuzz_media: at least one shard failed; see the logs above" >&2
  exit 1
fi

echo "tfdb_fuzz_media: $iterations iterations across $jobs shard(s), seeds" \
     "${seeds[*]}, no crash, hang, sanitizer report, or undocumented status"

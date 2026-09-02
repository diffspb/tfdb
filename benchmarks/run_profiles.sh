#!/usr/bin/env bash
set -euo pipefail

tool_dir=${TOOL_DIR:?TOOL_DIR is required}
records=${PROFILE_RECORDS:-50000}
repeats=${PROFILE_REPEATS:-1}
work_dir=$(mktemp -d /tmp/tfdb-profiles.XXXXXX)
trap 'rm -rf "$work_dir"' EXIT

common=(--size 16785408 --records "$records" --partition 4194304
  --index 262144 --block 32768 --quantum 4096
  --checkpoint-records 1000 --seed 20260901)

if ! [[ "$repeats" =~ ^[1-9][0-9]*$ ]]; then
  echo "PROFILE_REPEATS must be a positive integer" >&2
  exit 2
fi

run_profile() {
  local repeat=$1
  shift
  local output
  output=$("$tool_dir/tfdb_loadgen" "$@")
  printf 'repeat=%s %s\n' "$repeat" "$output"
}

uname -a
for ((repeat = 1; repeat <= repeats; ++repeat)); do
  repeat_dir="$work_dir/repeat-$repeat"
  mkdir -p "$repeat_dir"
  run_profile "$repeat" "$repeat_dir/tiny.tfdb" "${common[@]}" \
    --profile tiny --compression none
  run_profile "$repeat" "$repeat_dir/mixed.tfdb" "${common[@]}" \
    --profile mixed --compression none
  run_profile "$repeat" "$repeat_dir/compressible.tfdb" "${common[@]}" \
    --profile compressible --compression packbits
  run_profile "$repeat" "$repeat_dir/incompressible.tfdb" "${common[@]}" \
    --profile incompressible --compression packbits
  run_profile "$repeat" "$repeat_dir/logs.tfdb" "${common[@]}" \
    --profile logs --compression packbits
  run_profile "$repeat" "$repeat_dir/bad-time.tfdb" "${common[@]}" \
    --profile bad-time --compression none
  run_profile "$repeat" "$repeat_dir/burst.tfdb" "${common[@]}" \
    --profile burst --compression none --async --sync-ms 50
  run_profile "$repeat" "$repeat_dir/sparse.tfdb" --size 16785408 \
    --records 2000 --partition 4194304 --index 262144 --block 32768 \
    --quantum 4096 --checkpoint-records 1 --seed 20260901 \
    --profile sparse --compression none
  run_profile "$repeat" "$repeat_dir/hot-rotation.tfdb" --size 204800 \
    --records 5000 --partition 65536 --index 4096 --block 256 \
    --quantum 4096 --checkpoint-records 10 --seed 20260901 \
    --profile hot-rotation --compression none
  run_profile "$repeat" "$repeat_dir/concurrent-read.tfdb" --size 204800 \
    --records 5000 --partition 65536 --index 4096 --block 256 \
    --quantum 4096 --checkpoint-records 10 --seed 20260901 \
    --profile concurrent-read --compression none
done

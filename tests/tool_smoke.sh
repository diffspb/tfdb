#!/usr/bin/env bash
set -euo pipefail

tool_dir=${TOOL_DIR:?TOOL_DIR is required}
work_dir=$(mktemp -d /tmp/tfdb-tool-smoke.XXXXXX)
trap 'rm -rf "$work_dir"' EXIT

empty_store="$work_dir/empty.tfdb"
loaded_store="$work_dir/loaded.tfdb"
size=204800

"$tool_dir/tfdb_format" "$empty_store" --size "$size" \
  --partition 65536 --index 4096 --block 256 --quantum 4096 \
  >"$work_dir/format.out"
grep -q '^formatted ' "$work_dir/format.out"
"$tool_dir/tfdb_inspect" "$empty_store" >"$work_dir/empty-inspect.out"
grep -q 'partitions=3' "$work_dir/empty-inspect.out"
"$tool_dir/tfdb_verify" "$empty_store" >"$work_dir/empty-verify.out"
grep -q 'gaps=0' "$work_dir/empty-verify.out"

"$tool_dir/tfdb_loadgen" "$loaded_store" --size "$size" --records 50 \
  --profile mixed --partition 65536 --index 4096 --block 256 \
  --quantum 4096 --checkpoint-records 10 >"$work_dir/load.out"
grep -q 'records=50' "$work_dir/load.out"
grep -q 'durable_records=50' "$work_dir/load.out"
"$tool_dir/tfdb_verify" "$loaded_store" >"$work_dir/verify.out"
grep -q 'gaps=0' "$work_dir/verify.out"
"$tool_dir/tfdb_dump" "$loaded_store" --framed-v1 --from 0 \
  --to 1000000000 --selector 0 >"$work_dir/dump.out"
grep -q 'selector=0' "$work_dir/dump.out"
"$tool_dir/tfdb_dump" "$loaded_store" --raw-blocks >"$work_dir/raw.bin"
test -s "$work_dir/raw.bin"

# Exercise the load tool's independent persisted-write retention oracle across
# repeated slot replacement, not only on a volume that still retains ordinal 0.
rotating_store="$work_dir/rotating.tfdb"
"$tool_dir/tfdb_loadgen" "$rotating_store" --size "$size" --records 500 \
  --profile hot-rotation --partition 65536 --index 4096 --block 256 \
  --quantum 4096 --checkpoint-records 10 >"$work_dir/rotating-load.out"
grep -q 'records=500' "$work_dir/rotating-load.out"
if grep -q 'retained_first_ordinal=0 ' "$work_dir/rotating-load.out"; then
  echo "rotation smoke did not overwrite the oldest retained record" >&2
  exit 3
fi

reservoir_store="$work_dir/reservoir.tfdb"
"$tool_dir/tfdb_loadgen" "$reservoir_store" --size 16785408 \
  --records 65537 --profile tiny --partition 4194304 --index 262144 \
  --block 32768 --quantum 4096 --checkpoint-records 1000 \
  >"$work_dir/reservoir-load.out"
grep -q 'append_latency_observations=65537 append_latency_samples=65536' \
  "$work_dir/reservoir-load.out"

short_store="$work_dir/short-write.tfdb"
"$tool_dir/tfdb_loadgen" "$short_store" --size "$size" --records 50 \
  --profile mixed --partition 65536 --index 4096 --block 256 \
  --quantum 4096 --checkpoint-records 10 --backend-write-chunk 17 \
  >"$work_dir/short-write-load.out"
grep -q 'durable_records=50' "$work_dir/short-write-load.out"
grep -q 'retained_records=50' "$work_dir/short-write-load.out"

# The tools must make incomplete output observable rather than returning a
# misleading success. The first block payload begins at 8192 + 4096 + 128.
corrupt_store="$work_dir/corrupt.tfdb"
cp "$loaded_store" "$corrupt_store"
printf '\x00' | dd of="$corrupt_store" bs=1 seek=12416 conv=notrunc status=none
set +e
"$tool_dir/tfdb_verify" "$corrupt_store" >"$work_dir/corrupt-verify.out" \
  2>"$work_dir/corrupt-verify.err"
verify_status=$?
"$tool_dir/tfdb_dump" "$corrupt_store" --raw-blocks \
  >"$work_dir/corrupt-raw.bin" 2>"$work_dir/corrupt-dump.err"
dump_status=$?
set -e
test "$verify_status" -eq 3
test "$dump_status" -eq 3
grep -q 'gaps=1' "$work_dir/corrupt-verify.out"
grep -q '^gap ' "$work_dir/corrupt-dump.err"

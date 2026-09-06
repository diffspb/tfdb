#!/usr/bin/env bash
set -euo pipefail

: "${CPP_TOOL_DIR:?CPP_TOOL_DIR is required}"
: "${CONFORMANCE_WRITER:?CONFORMANCE_WRITER is required}"
: "${RUST_BIN_DIR:?RUST_BIN_DIR is required}"

case_dir=$(mktemp -d)
trap 'rm -rf "$case_dir"' EXIT

generated="$case_dir/valid-mixed.tfdb"
"$CONFORMANCE_WRITER" "$generated"
cmp testdata/format-v1/valid-mixed.tfdb "$generated"

set_byte() {
  local path=$1
  local offset=$2
  local octal=$3
  printf "%b" "\\$octal" | dd of="$path" bs=1 seek="$offset" conv=notrunc status=none
}

expect_exit() {
  local expected=$1
  shift
  set +e
  "$@" >"$case_dir/status.stdout" 2>"$case_dir/status.stderr"
  local actual=$?
  set -e
  if [[ $actual -ne $expected ]]; then
    printf 'expected exit %s, got %s: %s\n' "$expected" "$actual" "$*" >&2
    sed -n '1,10p' "$case_dir/status.stderr" >&2
    return 1
  fi
}

"$CPP_TOOL_DIR/tfdb_inspect" "$generated" >"$case_dir/cpp.inspect"
"$RUST_BIN_DIR/tfdb-rs-inspect" "$generated" >"$case_dir/rust.inspect"
cmp "$case_dir/cpp.inspect" "$case_dir/rust.inspect"

"$CPP_TOOL_DIR/tfdb_dump" "$generated" >"$case_dir/cpp.blocks"
"$RUST_BIN_DIR/tfdb-rs-dump" "$generated" >"$case_dir/rust.blocks"
cmp "$case_dir/cpp.blocks" "$case_dir/rust.blocks"

"$CPP_TOOL_DIR/tfdb_dump" "$generated" --framed-v1 --from -200 --to 1700 \
  --selector 10 --time-domain 42 >"$case_dir/cpp.records"
"$RUST_BIN_DIR/tfdb-rs-dump" "$generated" --framed-v1 --from -200 --to 1700 \
  --selector 10 --time-domain 42 >"$case_dir/rust.records"
cmp "$case_dir/cpp.records" "$case_dir/rust.records"

"$CPP_TOOL_DIR/tfdb_verify" "$generated" >"$case_dir/cpp.verify"
"$RUST_BIN_DIR/tfdb-rs-verify" "$generated" >"$case_dir/rust.verify"
grep -q '^verified blocks=4 raw_bytes=714 gaps=0$' "$case_dir/cpp.verify"
grep -q '^verified blocks=4 records=8 raw_bytes=714 gaps=0$' "$case_dir/rust.verify"

# Both implementations must assign the same contract-level result to derived
# corruption/recovery cases. Exact diagnostic prose is intentionally not ABI.
cp "$generated" "$case_dir/one-volume-copy.tfdb"
set_byte "$case_dir/one-volume-copy.tfdb" 0 130
expect_exit 0 "$CPP_TOOL_DIR/tfdb_verify" "$case_dir/one-volume-copy.tfdb"
expect_exit 0 "$RUST_BIN_DIR/tfdb-rs-verify" "$case_dir/one-volume-copy.tfdb"

cp "$case_dir/one-volume-copy.tfdb" "$case_dir/both-volume-copies.tfdb"
set_byte "$case_dir/both-volume-copies.tfdb" 4096 130
expect_exit 2 "$CPP_TOOL_DIR/tfdb_verify" "$case_dir/both-volume-copies.tfdb"
expect_exit 2 "$RUST_BIN_DIR/tfdb-rs-verify" "$case_dir/both-volume-copies.tfdb"

cp "$generated" "$case_dir/corrupt-index.tfdb"
set_byte "$case_dir/corrupt-index.tfdb" 65536 001
expect_exit 3 "$CPP_TOOL_DIR/tfdb_verify" "$case_dir/corrupt-index.tfdb"
expect_exit 3 "$RUST_BIN_DIR/tfdb-rs-verify" "$case_dir/corrupt-index.tfdb"

cp "$generated" "$case_dir/corrupt-payload.tfdb"
set_byte "$case_dir/corrupt-payload.tfdb" 12416 001
expect_exit 3 "$CPP_TOOL_DIR/tfdb_verify" "$case_dir/corrupt-payload.tfdb"
expect_exit 3 "$RUST_BIN_DIR/tfdb-rs-verify" "$case_dir/corrupt-payload.tfdb"

cp "$generated" "$case_dir/torn-active-tail.tfdb"
set_byte "$case_dir/torn-active-tail.tfdb" 77824 130
expect_exit 0 "$CPP_TOOL_DIR/tfdb_verify" "$case_dir/torn-active-tail.tfdb"
expect_exit 0 "$RUST_BIN_DIR/tfdb-rs-verify" "$case_dir/torn-active-tail.tfdb"
"$CPP_TOOL_DIR/tfdb_dump" "$case_dir/torn-active-tail.tfdb" >"$case_dir/cpp.torn"
"$RUST_BIN_DIR/tfdb-rs-dump" "$case_dir/torn-active-tail.tfdb" >"$case_dir/rust.torn"
cmp "$case_dir/cpp.torn" "$case_dir/rust.torn"

cp "$generated" "$case_dir/corrupt-partition-header.tfdb"
set_byte "$case_dir/corrupt-partition-header.tfdb" 8192 130
expect_exit 3 "$CPP_TOOL_DIR/tfdb_verify" "$case_dir/corrupt-partition-header.tfdb"
expect_exit 3 "$RUST_BIN_DIR/tfdb-rs-verify" "$case_dir/corrupt-partition-header.tfdb"

cp "$generated" "$case_dir/unsupported-major.tfdb"
set_byte "$case_dir/unsupported-major.tfdb" 8 002
set_byte "$case_dir/unsupported-major.tfdb" 4104 002
expect_exit 2 "$CPP_TOOL_DIR/tfdb_verify" "$case_dir/unsupported-major.tfdb"
grep -q '^unsupported:' "$case_dir/status.stderr"
expect_exit 2 "$RUST_BIN_DIR/tfdb-rs-verify" "$case_dir/unsupported-major.tfdb"
grep -q 'Unsupported:' "$case_dir/status.stderr"

cp "$generated" "$case_dir/truncated.tfdb"
truncate -s 100000 "$case_dir/truncated.tfdb"
expect_exit 2 "$CPP_TOOL_DIR/tfdb_verify" "$case_dir/truncated.tfdb"
expect_exit 2 "$RUST_BIN_DIR/tfdb-rs-verify" "$case_dir/truncated.tfdb"

printf '%s\n' 'cross-language conformance: PASS'

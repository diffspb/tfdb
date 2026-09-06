# TFDB repository context

This file applies to the whole repository. Read it before changing TFDB.

## Project state

- TFDB means Telemetry Flash DB.
- `v1.0.0` at commit `353ee2b` is the immutable C++14 source-release
  baseline.
- The on-media format described by `docs/format-v1.md` is still a candidate,
  not a frozen interoperability promise.
- The next milestone is format-v1 qualification and freeze, described in
  `docs/roadmap.md`.
- Current software evidence is in `docs/evidence.md`. Do not generalize the
  WSL2 reference numbers into target-device guarantees.

## Read in this order

1. `docs/roadmap.md` for current priorities, gates, and machine handoff.
2. `docs/architecture.md` and `docs/decisions.md` for design invariants.
3. `docs/format-v1.md` before any persistent-format change.
4. `docs/testing.md` and `docs/evidence.md` before changing recovery,
   durability, concurrency, or performance claims.
5. `docs/api.md` and `docs/tutorial.md` for the public integration contract.

Use `skills/tfdb-integration/SKILL.md` when embedding TFDB into an application.

## Non-negotiable invariants

- Keep the runtime library C++14 and dependency-free unless an explicit
  architecture decision changes that constraint.
- Preserve one writer, physical-order readers, bounded storage, sequential
  block writes, rebuildable indexes, and partition-local configuration.
- Treat `publish()` as visibility only. Durability requires a successful
  checkpoint, close, or completed rotation and ultimately depends on the
  backend/device flush contract.
- Never discard anomalous-time records. Keep the application-provided index
  clock separate from source timestamps retained in the payload.
- Readers never pin rotation; report overwrite/corruption gaps explicitly.
- Do not change encoded bytes, IDs, sizes, endianness, CRC coverage, recovery
  selection, or reserved fields without new golden vectors, an ADR, and
  independent-reader review.
- Do not claim a loss window, retention period, or eight-year lifetime without
  measurements from the exact target storage stack.

## Change and verification discipline

- Include only headers under `include/tfdb` from consuming applications.
- Check every `Status`; preserve the original write/flush/background failure.
- Keep formatting/provisioning separate from normal open/recovery.
- Add deterministic fault/crash oracles for storage-state changes, not only
  happy-path tests.
- Run the relevant narrow test first. Before a release or format decision run:

```sh
make check
make crash-matrix
make sanitize
make coverage
make cxx17-check
make fuzz
python3 docs/build_html.py --check
```

`make fuzz` defaults to a short run. Before a release or format decision also
run a long one with a fresh seed, for example
`make fuzz FUZZ_ITERATIONS=500000 FUZZ_SEED=$(date +%s)`, and record the
iteration count and seed in `docs/evidence.md`. A saved failing image that
exposes a specification gap belongs in `testdata/format-v1/` with an expected
classification, not only in the fuzz harness.

Also run the structural validator supplied by the local skill-creator
environment against `skills/tfdb-integration`; its installation path is not a
repository contract.

- Run `make tsan` and the long reader/writer soak on native Linux. A WSL2
  ThreadSanitizer runtime failure is not a pass.
- Regenerate committed HTML with `python3 docs/build_html.py` after changing
  `README.md` or any mapped Markdown document.
- Keep benchmark raw output, its summary, source-bundle digest, and evidence
  document mutually consistent. Do not replace preserved outliers with a
  cleaner run.
- Never re-tag or rewrite `v1.0.0`; place subsequent work on a new commit or
  branch.

## Current priority

The initial independent Rust read-only implementation and shared deterministic
corpus are present. Extend the shared negative corpus with feature/bounds,
stale-writer-chain, and live-rotation cases, then move testing to native Linux
for fuzzing, TSan, and the long soak while preparing representative trace
replay. Do not add optional codecs or secondary indexes. Physical power-cut
and endurance work begins only on an explicitly identified, expendable target
device with an approved test procedure.

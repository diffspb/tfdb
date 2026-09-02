#!/usr/bin/env bash
set -euo pipefail

object_dir=${OBJECT_DIR:?OBJECT_DIR is required}
min_line=${MIN_LINE_COVERAGE:-85}
min_outcome=${MIN_BRANCH_OUTCOME_COVERAGE:-48}
sources=(
  src/status.cpp src/storage_posix.cpp src/memory_storage.cpp
  src/counting_storage.cpp src/codec.cpp src/internal_format.cpp
  src/ring_store.cpp src/record_profile.cpp src/async_writer.cpp
)

# "Branches executed" only means that a branch expression was evaluated.
# "Taken at least once" counts the individual control-flow outcomes and is the
# meaningful gate. Per-file floors prevent a strong core file from hiding a
# weak backend or async implementation.
gcov -n -b -c -o "$object_dir" "${sources[@]}" 2>/dev/null |
awk -v min_line="$min_line" -v min_outcome="$min_outcome" '
  function percent(field, prefix) {
    sub("^" prefix ":", "", field); sub(/%$/, "", field); return field + 0;
  }
  function file_min_line(name) {
    if (name == "storage_posix.cpp") return 75;
    if (name == "ring_store.cpp" || name == "internal_format.cpp") return 90;
    if (name == "async_writer.cpp" || name == "record_profile.cpp" ||
        name == "memory_storage.cpp" || name == "codec.cpp") return 85;
    return 90;
  }
  function file_min_outcome(name) {
    if (name == "storage_posix.cpp") return 35;
    if (name == "ring_store.cpp" || name == "async_writer.cpp" ||
        name == "record_profile.cpp" || name == "codec.cpp") return 50;
    if (name == "internal_format.cpp" || name == "memory_storage.cpp") return 45;
    return 50;
  }
  /^File / {
    current = "";
    if ($0 ~ /^File '\''src\/[A-Za-z0-9_]+\.cpp'\''$/) {
      current = $0;
      sub(/^File '\''src\//, "", current);
      sub(/'\''$/, "", current);
    }
    next;
  }
  current != "" && /^Lines executed:/ {
    line_percent = percent($2, "executed"); line_count = $4 + 0;
    line_total += line_count; line_hit += line_percent * line_count / 100;
    next;
  }
  current != "" && /^Branches executed:/ {
    evaluated_percent = percent($2, "executed"); branch_count = $4 + 0;
    branch_evaluated += evaluated_percent * branch_count / 100;
    next;
  }
  current != "" && /^Taken at least once:/ {
    outcome_percent = percent($4, "once"); outcome_count = $6 + 0;
    branch_total += outcome_count;
    branch_taken += outcome_percent * outcome_count / 100;
    required_line = file_min_line(current);
    required_outcome = file_min_outcome(current);
    printf "coverage_file=%s lines=%.1f%%/%.1f%% branch_outcomes=%.1f%%/%.1f%%\n", \
      current, line_percent, required_line, outcome_percent, required_outcome;
    if (line_percent + 0.0001 < required_line ||
        outcome_percent + 0.0001 < required_outcome) failed = 1;
    current = "";
    next;
  }
  END {
    aggregate_line = 100 * line_hit / line_total;
    aggregate_evaluated = 100 * branch_evaluated / branch_total;
    aggregate_outcome = 100 * branch_taken / branch_total;
    printf "production_line_coverage=%.1f%% (%d lines; minimum %.1f%%)\n", \
      aggregate_line, line_total, min_line;
    printf "production_branches_evaluated=%.1f%% (%d branch expressions; diagnostic only)\n", \
      aggregate_evaluated, branch_total;
    printf "production_branch_outcomes_taken=%.1f%% (%d outcomes; minimum %.1f%%)\n", \
      aggregate_outcome, branch_total, min_outcome;
    if (aggregate_line + 0.0001 < min_line ||
        aggregate_outcome + 0.0001 < min_outcome || failed) exit 1;
  }
'

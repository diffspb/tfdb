#!/usr/bin/env bash
set -euo pipefail

cmake_program=${CMAKE:-cmake}
ctest_program=${CTEST:-ctest}
meson_program=${MESON:-meson}
ninja_program=${NINJA:-ninja}

root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

"$cmake_program" -S "$root" -B "$work/cmake" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$ninja_program" \
  -DCMAKE_INSTALL_PREFIX="$work/cmake-prefix"
"$cmake_program" --build "$work/cmake"
"$ctest_program" --test-dir "$work/cmake" --output-on-failure
"$cmake_program" --install "$work/cmake"
"$cmake_program" -S "$root/tests/consumer" -B "$work/cmake-consumer" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$ninja_program" \
  -DCMAKE_PREFIX_PATH="$work/cmake-prefix"
"$cmake_program" --build "$work/cmake-consumer"
"$work/cmake-consumer/tfdb_consumer"

"$meson_program" setup "$work/meson" "$root" \
  --prefix="$work/meson-prefix"
"$meson_program" compile -C "$work/meson"
"$meson_program" test -C "$work/meson" --print-errorlogs
"$meson_program" install -C "$work/meson"
pkgconfig_dir=$(find "$work/meson-prefix" -type d -name pkgconfig -print -quit)
test -n "$pkgconfig_dir"
PKG_CONFIG_PATH="$pkgconfig_dir" \
  "$meson_program" setup "$work/meson-consumer" "$root/tests/consumer"
PKG_CONFIG_PATH="$pkgconfig_dir" \
  "$meson_program" compile -C "$work/meson-consumer"
"$work/meson-consumer/tfdb_consumer"

for prefix in "$work/cmake-prefix" "$work/meson-prefix"; do
  cmp "$root/LICENSE" "$prefix/share/doc/tfdb/LICENSE"
  cmp "$root/CHANGELOG.md" "$prefix/share/doc/tfdb/CHANGELOG.md"
done

printf '%s\n' 'CMake/Meson build, install, and consumer smoke: PASS'

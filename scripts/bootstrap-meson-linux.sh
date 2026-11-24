#!/usr/bin/env bash
set -euo pipefail
root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${LIGHTUSD_MESON_BUILD_DIR:-$root_dir/build_ninja/meson-core}"
CXX="${CXX:-clang++}" meson setup "$build_dir" "$root_dir" \
  --buildtype debug -Db_sanitize=address "$@"

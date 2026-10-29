#!/usr/bin/env bash
# Compare serialized CPU graded outputs from two built tinyvdb revisions.
set -euo pipefail
if [ "$#" -ne 5 ]; then
  echo "usage: $0 old-src old-build new-src new-build spacing" >&2
  exit 2
fi
old_src=$1 old_build=$2 new_src=$3 new_build=$4 spacing=$5
here=$(cd -- "$(dirname -- "$0")" && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/tinyvdb-graded-identity.XXXXXXXX")
trap 'rm -rf -- "$tmp"' EXIT
for revision in old new; do
  if [ "$revision" = old ]; then src=$old_src; build=$old_build; else src=$new_src; build=$new_build; fi
  "${CC:-cc}" -std=c11 -O2 -I"$src/src" "$here/graded-identity.c" \
    "$build/libtinyvdb_mesh_ops.a" "$build/libtinyvdb.a" -lm -pthread -o "$tmp/$revision"
done
for shape in 0 1 2; do
  nice "$tmp/old" "$shape" "$spacing" "$tmp/old.bin"
  nice "$tmp/new" "$shape" "$spacing" "$tmp/new.bin"
  sha256sum "$tmp/old.bin" "$tmp/new.bin"
  cmp "$tmp/old.bin" "$tmp/new.bin"
done

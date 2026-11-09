#!/usr/bin/env bash
# Compare two already-built tinyvdb revisions on the same host and fixture.
set -euo pipefail
if [ "$#" -ne 6 ]; then
  echo "usage: $0 old-src old-build new-src new-build level-set.vdb spacing" >&2
  exit 2
fi
old_src=$1 old_build=$2 new_src=$3 new_build=$4 input=$5 spacing=$6
here=$(cd -- "$(dirname -- "$0")" && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/tinyvdb-identity.XXXXXXXX")
trap 'rm -rf -- "$tmp"' EXIT
for revision in old new; do
  if [ "$revision" = old ]; then src=$old_src; build=$old_build; else src=$new_src; build=$new_build; fi
  "${CC:-cc}" -std=c11 -O2 -I"$src/src" "$here/mesh-identity.c" \
    "$build/libtinyvdb_mesh_ops.a" "$build/libtinyvdb.a" -lm -pthread \
    -o "$tmp/$revision"
  nice "$tmp/$revision" "$input" "$spacing" "$tmp/$revision.bin"
done
sha256sum "$tmp/old.bin" "$tmp/new.bin"
if cmp -s "$tmp/old.bin" "$tmp/new.bin"; then
  echo 'mesher and tet-mesh output: byte-identical'
else
  echo 'mesher or tet-mesh output differs' >&2
  exit 1
fi

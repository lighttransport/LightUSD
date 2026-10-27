#!/usr/bin/env bash
#
# pxr crate-fidelity check: does OpenUSD read the USDC we write as the same
# layer it reads from the source?
#
# run-usdcat-compare.sh compares our *reader* against pxr's (lusdcat vs usdcat
# on the same input) and asks usdchecker whether our crates validate like the
# source. It never asks pxr to read our crates and report what it sees, so a
# crate-encoding gap that our own reader tolerates stays invisible: the tests
# below were found exactly that way (variant specs without `properties`,
# compressed string/token index arrays, ValueBlock instead of an empty list-op
# for `= None`, `relocates` vs `layerRelocates`, shader inputs with no writer).
#
# For each fixture: write our crates with lusdcat, then let usdcat print both
# the source and our crate and diff the two prints. Byte-identical is the
# expected result; textual differences are re-checked with tests/compare-usda.js
# so spec/property ordering ("text-only") is separated from real data loss.
#
# Usage:
#   tests/run-pxr-crate-fidelity.sh                 # tests/usda/*.usda
#   tests/run-pxr-crate-fidelity.sh 'tests/usdc/*.usdc'
#   SHOW=40 tests/run-pxr-crate-fidelity.sh tests/usda/foo.usda   # print diffs
#
# Environment:
#   LUSDCAT_PATH  lusdcat binary (default ./build/lusdcat)
#   USDCAT_PATH   OpenUSD usdcat; defaults to <repo>/ref/dist/bin/usdcat, then
#                 a sibling ../OpenUSD/dist/bin/usdcat
#   NODE          node binary used for the text-only classification (default node)
#
# Exit status: 0 when no semantic difference is found, 1 otherwise, 2 when a
# tool is missing.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

LUSDCAT_PATH="${LUSDCAT_PATH:-$ROOT_DIR/build/lusdcat}"
if [ -z "${USDCAT_PATH:-}" ]; then
  if [ -x "$ROOT_DIR/ref/dist/bin/usdcat" ]; then
    USDCAT_PATH="$ROOT_DIR/ref/dist/bin/usdcat"
  elif [ -x "$ROOT_DIR/../OpenUSD/dist/bin/usdcat" ]; then
    USDCAT_PATH="$ROOT_DIR/../OpenUSD/dist/bin/usdcat"
  else
    USDCAT_PATH=""
  fi
fi
NODE="${NODE:-node}"
COMPARE="$SCRIPT_DIR/compare-usda.js"
PATTERN="${1:-$ROOT_DIR/tests/usda/*.usda}"

if [ -z "$USDCAT_PATH" ]; then
  echo "Error: OpenUSD usdcat not found. Build it with" >&2
  echo "  scripts/build-openusd-usdcat.sh" >&2
  echo "or set USDCAT_PATH." >&2
  exit 2
fi
for exe in "$LUSDCAT_PATH" "$USDCAT_PATH"; do
  if [ ! -x "$exe" ]; then
    echo "Error: not executable: $exe" >&2
    exit 2
  fi
done
if [ ! -f "$COMPARE" ]; then
  echo "Error: $COMPARE not found (text-only classification needs it)." >&2
  exit 2
fi
if ! command -v "$NODE" >/dev/null 2>&1; then
  echo "Warning: node not found; reporting raw textual differences only." >&2
  NODE=""
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "lusdcat: $LUSDCAT_PATH"
echo "usdcat:  $USDCAT_PATH"
echo "pattern: $PATTERN"
echo ""

same=0
textual=0
semantic=0
writefail=0
parsefail=0
declare -a TEXTUAL_FILES=()
declare -a SEMANTIC_FILES=()

shopt -s nullglob
for f in $PATTERN; do
  base="$(basename "$f")"
  crate="$TMP/$base.usdc"

  # Fixtures lusdcat cannot write standalone (missing reference targets, ...)
  # are covered by run-usdcat-compare.sh's usdchecker pass instead.
  if ! "$LUSDCAT_PATH" --output-format usdc -o "$crate" "$f" >/dev/null 2>&1; then
    writefail=$((writefail + 1))
    continue
  fi
  if ! "$USDCAT_PATH" "$f" > "$TMP/a.usda" 2>/dev/null; then
    parsefail=$((parsefail + 1))   # pxr cannot read the source at all
    rm -f "$crate"
    continue
  fi
  if ! "$USDCAT_PATH" "$crate" > "$TMP/b.usda" 2>/dev/null; then
    semantic=$((semantic + 1))
    SEMANTIC_FILES+=("$base (crate unreadable by usdcat)")
    [ -n "${SHOW:-}" ] && { echo "=== $base: crate unreadable by usdcat"; }
    rm -f "$crate"
    continue
  fi

  if cmp -s "$TMP/a.usda" "$TMP/b.usda"; then
    same=$((same + 1))
  elif [ -n "$NODE" ] && "$NODE" "$COMPARE" "$TMP/a.usda" "$TMP/b.usda" >/dev/null 2>&1; then
    textual=$((textual + 1))
    TEXTUAL_FILES+=("$base")
    if [ -n "${SHOW:-}" ]; then
      echo "=== $base (text-only, spec/property order)"
      diff "$TMP/a.usda" "$TMP/b.usda" | head -"${SHOW}"
    fi
  else
    semantic=$((semantic + 1))
    SEMANTIC_FILES+=("$base")
    if [ -n "${SHOW:-}" ]; then
      echo "=== $base"
      if [ -n "$NODE" ]; then
        "$NODE" "$COMPARE" --detailed-diff "$TMP/a.usda" "$TMP/b.usda" 2>&1 |
          sed -n '/difference(s)/,$p' | head -"${SHOW}"
      else
        diff "$TMP/a.usda" "$TMP/b.usda" | head -"${SHOW}"
      fi
    fi
  fi
  rm -f "$crate"
done

total=$((same + textual + semantic + writefail + parsefail))
echo "pxr crate fidelity: $same byte-identical, $textual text-only, $semantic semantic," \
     "$writefail unwritable, $parsefail pxr-cannot-parse (of $total)"
if [ ${#SEMANTIC_FILES[@]} -gt 0 ] && [ -z "${SHOW:-}" ]; then
  echo "  semantic differences:"
  printf '    %s\n' "${SEMANTIC_FILES[@]}"
fi

[ "$semantic" -eq 0 ] || exit 1
exit 0

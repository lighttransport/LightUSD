#!/usr/bin/env bash
#
# Driver-level regression for variant selection across a reference -> payload ->
# reference chain (the Pixar Kitchen_set Chair.usd shape).
#
# The C++ feat-variant-payload-chain test drives the COMPOSITION LIBRARY with its
# own deferred iteration loop; it does NOT exercise the binary flatten *drivers*.
# This runs each driver binary end-to-end so that reverting the variant-deferral
# in a driver loop — or in the shared ShouldDeferVariantComposition() helper —
# is caught: without deferral the strong local selection "ChairB" is consumed
# against empty variant blocks and the deep default "ChairA" wins.
#
# Covers:
#   - lusdcat       (examples/lusdcat, the native flatten driver)
#   - lusdzconvert  (tools/lusdzconvert -> src/usdz/usdz-converter.cc flatten loop)
#
# Usage: run-variant-chain.sh <lusdcat> <project-source-dir> [lusdzconvert]

set -u

LUSDCAT="${1:?usage: run-variant-chain.sh <lusdcat> <srcdir> [lusdzconvert]}"
SRCDIR="${2:?usage: run-variant-chain.sh <lusdcat> <srcdir> [lusdzconvert]}"
LUSDZCONVERT="${3:-}"
MAIN="${SRCDIR}/tests/usda/feat-variant-chain-main.usda"

if [ ! -f "${MAIN}" ]; then
  echo "FAIL: fixture not found: ${MAIN}"
  exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# check_flattened <label> <flattened-usda>: the output must carry the selected
# (non-default) variant's opinion and must NOT carry the default variant's
# opinion (variant sets are consumed on flatten, so the unselected block does not
# leak). Returns 0 on success, 1 on failure.
check_flattened() {
  local label="$1" out="$2"
  if [ ! -s "${out}" ]; then
    echo "FAIL: ${label}: produced no/empty output"
    return 1
  fi
  if grep -q 'I_am_ChairB' "${out}" && ! grep -q 'I_am_ChairA' "${out}"; then
    echo "PASS: ${label}: selected non-default variant ChairB across ref->payload->ref chain"
    return 0
  fi
  echo "FAIL: ${label}: expected I_am_ChairB (and not I_am_ChairA) in flattened output."
  echo "--- 'which' opinions ---"
  grep 'which' "${out}" || echo "(none)"
  return 1
}

rc=0

# --- lusdcat ---
if [ -x "${LUSDCAT}" ]; then
  OUT="${WORK}/lusdcat.usda"
  if "${LUSDCAT}" --flatten "${MAIN}" -o "${OUT}" >/dev/null 2>&1; then
    check_flattened "lusdcat" "${OUT}" || rc=1
  else
    echo "FAIL: lusdcat --flatten failed on ${MAIN}"; rc=1
  fi
else
  echo "SKIP: lusdcat not found at ${LUSDCAT}"
fi

# --- lusdzconvert (src/usdz/usdz-converter.cc flatten path) ---
if [ -n "${LUSDZCONVERT}" ] && [ -x "${LUSDZCONVERT}" ]; then
  OUT="${WORK}/lusdzconvert.usda"
  if "${LUSDZCONVERT}" "${MAIN}" "${OUT}" --outputFormat usda >/dev/null 2>&1; then
    check_flattened "lusdzconvert" "${OUT}" || rc=1
  else
    echo "FAIL: lusdzconvert flatten failed on ${MAIN}"; rc=1
  fi
elif [ -n "${LUSDZCONVERT}" ]; then
  echo "SKIP: lusdzconvert not found at ${LUSDZCONVERT}"
fi

# --- LIVRPS strength: inherits beats references (L > I > ... > R) ---
# lusdcat default --flatten routes through CompositeAllArcs; the legacy
# per-feature loop applied R before I and let the referenced opinion win.
LIVRPS_MAIN="${SRCDIR}/tests/usda/feat-livrps-inherits-main.usda"
if [ -x "${LUSDCAT}" ] && [ -f "${LIVRPS_MAIN}" ]; then
  OUT="${WORK}/lusdcat-livrps.usda"
  if "${LUSDCAT}" --flatten "${LIVRPS_MAIN}" -o "${OUT}" >/dev/null 2>&1; then
    if grep -q "int x = 50" "${OUT}" && ! grep -q "int x = 100" "${OUT}"; then
      echo "PASS: lusdcat: inherited class opinion (x=50) beats referenced (x=100)"
    else
      echo "FAIL: lusdcat: expected inherits (x=50) to beat references (x=100)."
      rc=1
    fi
  else
    echo "FAIL: lusdcat --flatten failed on ${LIVRPS_MAIN}"; rc=1
  fi
fi

exit ${rc}

#!/usr/bin/env bash
# An absent backend may skip; renderer bugs and allocation failures must fail.
set -euo pipefail
HELPER="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/gpu-test-common.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
check_status() {
  local expected="$1" backend="$2" status="$3" message="$4" actual
  printf '%s\n' "$message" > "$TMP/render.log"
  if bash -c 'source "$1"; lusdrender_test_render_failed "$2" "$3" "$4"' \
      _ "$HELPER" "$backend" "$TMP/render.log" "$status" > "$TMP/result.log" 2>&1; then
    actual=0
  else
    actual=$?
  fi
  if [ "$actual" -ne "$expected" ]; then
    cat "$TMP/result.log"
    echo "FAIL: expected status $expected, got $actual"
    exit 1
  fi
}
check_status 77 vkr 1 'Failed to create LightRT Vulkan engine (err=4).'
check_status 77 vkr 1 'Failed to create LightRT Vulkan engine (err=8).'
check_status 77 vkr 1 'Failed to create LightRT Vulkan engine for mesh chunks (err=4).'
check_status 1 cpu 1 'Failed to create LightRT Vulkan engine (err=4).'
check_status 1 vkr 1 'Failed to create LightRT Vulkan engine (err=2).'
check_status 1 vkr 139 'Failed to create LightRT Vulkan engine (err=4).'
check_status 1 vkr 1 $'Failed to create LightRT Vulkan engine (err=4).\nValidation Error: VUID-test'
check_status 1 vkr 1 $'Failed to create LightRT Vulkan engine (err=4).\nVK_ERROR_DEVICE_LOST'
check_status 1 vkr 1 'Vulkan ray-query failed (rc=2); refusing further work on the failed context.'
check_status 1 vkr 1 'Failed to parse scene'
echo 'PASS: only unavailable Vulkan startup skips; all other errors fail'

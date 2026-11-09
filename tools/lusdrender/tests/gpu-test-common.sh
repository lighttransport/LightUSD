#!/usr/bin/env bash
# Called only after a render fails; unavailable backends skip, other failures fail.
lusdrender_test_render_failed() {
  local backend="$1" log="$2" status="$3"
  cat "$log"
  if [ "$backend" = vkr ] && [ "$status" -eq 1 ] &&
     ! grep -Eq 'Validation Error|VUID-|VK_ERROR_DEVICE_LOST|failed-context' "$log" &&
     grep -Eq '^Failed to create LightRT Vulkan engine( for mesh chunks)? \(err=(4|8)\)\.$' "$log"; then
    echo 'SKIP: Vulkan backend or device unavailable'
    exit 77
  fi
  echo "FAIL: renderer exited with status $status"
  exit 1
}

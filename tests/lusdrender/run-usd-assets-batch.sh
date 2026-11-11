#!/usr/bin/env bash
#
# Batch-render a USD asset corpus through lusdrender only (CPU ray preview +
# Vulkan). Thin wrapper over the shared harness
# examples/lusdview/tests/run-usd-assets-render-smoke.sh: it bakes lusdrender-only
# modes and sane batch defaults, and forwards any extra args (--root / --limit /
# --modes / --timeout / --out / --golden ...).
#
# Corpus: set USD_ASSETS_ROOT (or --root DIR). Defaults to the local
# usd-wg/assets checkout at /mnt/disk1/work/usd-assets; the harness SKIPs if it
# does not exist. lusdrender's core loader allows '..' parent-relative
# references by default, so no allow-parent-paths flag is needed here.
#
# Modes: lusdr-cpu (-rtPreview), lusdr-vk (-vk), lusdr-vkr (-vkr).
#
# Examples:
#   tests/lusdrender/run-usd-assets-batch.sh
#   tests/lusdrender/run-usd-assets-batch.sh --root ~/usd-assets --limit 30
#   tests/lusdrender/run-usd-assets-batch.sh --modes lusdr-cpu --out /tmp/tr-batch
#
# For the combined lusdview+lusdrender sweep see
# examples/lusdview/tests/run-usd-assets-disk1.sh (ctest: lusdview-usd-assets-disk1).
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

: "${USD_ASSETS_ROOT:=${USD_WG_ASSETS_DIR:-}}"
: "${LUSDRENDER:=$REPO_ROOT/build/tools/lusdrender/lusdrender}"
# The shared harness SKIPs when the lusdview binary is absent (even for
# lusdrender-only modes), so point LUSDVIEW at the build path too; it is never
# invoked for lusdr-* modes.
: "${LUSDVIEW:=$REPO_ROOT/build/lusdview}"
: "${LUSDVIEW_USD_ASSETS_MODES:=lusdr-cpu,lusdr-vk}"
: "${LUSDVIEW_XVFB:=1}"
: "${LUSDVIEW_USD_ASSETS_TIMEOUT:=60s}"
# Report unsupported/empty assets, but only fail on crashes/hangs/backend breakage.
: "${LUSDVIEW_USD_ASSETS_FAIL_ON:=timeout,backend_error}"

export USD_ASSETS_ROOT LUSDRENDER LUSDVIEW LUSDVIEW_USD_ASSETS_MODES \
  LUSDVIEW_XVFB LUSDVIEW_USD_ASSETS_TIMEOUT LUSDVIEW_USD_ASSETS_FAIL_ON

exec bash "$REPO_ROOT/examples/lusdview/tests/run-usd-assets-render-smoke.sh" "$@"

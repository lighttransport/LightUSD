#!/usr/bin/env bash
#
# Convenience wrapper: batch-render the local usd-wg/assets checkout at
# /mnt/disk1/work/usd-assets through BOTH lusdview and lusdrender.
#
# This just bakes the asset root, both-tool mode set, and sane batch defaults
# into run-usd-assets-render-smoke.sh (the real harness). Any extra args are
# forwarded to that harness, so you can override e.g. --modes / --limit /
# --timeout / --out. Examples:
#
#   examples/lusdview/tests/run-usd-assets-disk1.sh
#   examples/lusdview/tests/run-usd-assets-disk1.sh --limit 20
#   examples/lusdview/tests/run-usd-assets-disk1.sh --modes vk-rt,lusdr-vk --out /tmp/batch
#
# Same test is registered in ctest as `lusdview-usd-assets-disk1-smoke`
# (opt-in: LUSDVIEW_USD_ASSETS_DISK1=1 ctest -R lusdview-usd-assets-disk1).
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

: "${USD_ASSETS_ROOT:=/mnt/disk1/work/usd-assets}"
: "${LUSDVIEW:=$REPO_ROOT/build/lusdview}"
: "${LUSDRENDER:=$REPO_ROOT/build/tools/lusdrender/lusdrender}"
: "${LUSDVIEW_USD_ASSETS_MODES:=vk-raster,vk-rt,lusdr-cpu,lusdr-vk}"
# usd-wg/assets uses '..' parent-relative references (e.g. ../_common/*.usda);
# lusdview rejects those by default (security policy), so allow them here to
# match lusdrender's core-path default and actually load these assets.
: "${LUSDVIEW_USD_ASSETS_ALLOW_PARENT:=1}"
: "${LUSDVIEW_XVFB:=1}"
: "${LUSDVIEW_USD_ASSETS_TIMEOUT:=60s}"
# Report unsupported/empty assets, but only fail on crashes/hangs/backend breakage.
: "${LUSDVIEW_USD_ASSETS_FAIL_ON:=timeout,backend_error}"

export USD_ASSETS_ROOT LUSDVIEW LUSDRENDER LUSDVIEW_USD_ASSETS_MODES \
  LUSDVIEW_USD_ASSETS_ALLOW_PARENT LUSDVIEW_XVFB LUSDVIEW_USD_ASSETS_TIMEOUT \
  LUSDVIEW_USD_ASSETS_FAIL_ON

exec bash "$SCRIPT_DIR/run-usd-assets-render-smoke.sh" "$@"

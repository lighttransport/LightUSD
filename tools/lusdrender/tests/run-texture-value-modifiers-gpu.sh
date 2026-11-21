#!/usr/bin/env bash
# Run the analytic RGB channel contract through real Vulkan primary tracing.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec bash "$HERE/run-texture-value-modifiers.sh" "${1:?lusdrender required}" vkr

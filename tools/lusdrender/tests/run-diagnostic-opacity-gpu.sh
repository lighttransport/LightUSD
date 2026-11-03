#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec bash "$HERE/run-diagnostic-opacity.sh" "${1:?lusdrender required}" vkr

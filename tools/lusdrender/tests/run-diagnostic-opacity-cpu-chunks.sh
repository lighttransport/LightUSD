#!/usr/bin/env bash
set -euo pipefail
exec bash "$(dirname "$0")/run-diagnostic-opacity.sh" "${1:?lusdrender required}" cpu -triangleChunk 1

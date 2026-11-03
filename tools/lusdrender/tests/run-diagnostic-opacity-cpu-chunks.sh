#!/usr/bin/env bash
set -euo pipefail
export LUSDR_TRIANGLE_CHUNK=1
exec bash "$(dirname "$0")/run-diagnostic-opacity.sh" "${1:?lusdrender required}" cpu

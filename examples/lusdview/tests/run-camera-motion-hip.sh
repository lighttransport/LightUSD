#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LUSDVIEW_MOTION_BACKEND=hip
exec "$SCRIPT_DIR/run-camera-motion-cpu.sh" "$@"

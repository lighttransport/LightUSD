#!/usr/bin/env bash
# Compatibility entry point; the structured harness owns parity assertions.
set -euo pipefail
exec python3 "$(dirname "$0")/checker-parity.py" "$@"

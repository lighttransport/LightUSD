#!/usr/bin/env bash
set -euo pipefail
exec bash "$(dirname "$0")/run-diagnostic-material.sh" "${1:?lusdrender required}" vkr

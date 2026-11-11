#!/usr/bin/env bash
set -euo pipefail
# Run from the native build directory; override for a standalone src build.
ClangBuildAnalyzer --start "${LIGHTUSD_TRACE_OBJECT_DIR:-src/CMakeFiles/lightusd_core.dir}"

#!/usr/bin/env bash
set -euo pipefail
ClangBuildAnalyzer --stop "${LIGHTUSD_TRACE_OBJECT_DIR:-src/CMakeFiles/lightusd_core.dir}" "${LIGHTUSD_TRACE_CAPTURE:-capturefile}"

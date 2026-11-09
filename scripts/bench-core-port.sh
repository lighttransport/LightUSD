#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2024-Present Light Transport Entertainment Inc.
# Measure a clean C11 core/product build using the common measurement harness.
# Reports are JSON. Configure with CMAKE_EXPORT_COMPILE_COMMANDS=ON first.
# --wasm selects lightusd_next_wasm; --artifact may add any linked artifact.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build_ninja"
OUTPUT=""
JOBS=16
REPEAT=3
TARGET=lightusd_next
SIZE_TOOL=""
ARTIFACTS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --output) OUTPUT="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --repeat) REPEAT="$2"; shift 2 ;;
    --target) TARGET="$2"; shift 2 ;;
    --size-tool) SIZE_TOOL="$2"; shift 2 ;;
    --artifact) ARTIFACTS+=(--artifact "$2"); shift 2 ;;
    --wasm) TARGET=lightusd_next_wasm; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
OUTPUT="${OUTPUT:-${BUILD_DIR}/core-port.json}"
# Discover the objects and linked output from this tree, never a neighboring
# build or the application's potentially stale installed WASM module.
# A temporary manifest also propagates discovery errors (process substitution
# alone would hide them from set -e).
MANIFEST="$(mktemp "${BUILD_DIR}/core-port-artifacts.XXXXXX")"
trap 'rm -f "$MANIFEST"' EXIT
python3 - "$BUILD_DIR" "$TARGET" "$SIZE_TOOL" > "$MANIFEST" <<'PY'
import json, pathlib, shlex, shutil, subprocess, sys
build, target = pathlib.Path(sys.argv[1]), sys.argv[2]
entries = json.loads((build / 'compile_commands.json').read_text())
size_tool = sys.argv[3] or shutil.which('llvm-size')
if not size_tool:
    for entry in entries:
        args = entry.get('arguments') or shlex.split(entry['command'])
        candidate = pathlib.Path(args[0]).resolve().parent.parent / 'bin/llvm-size'
        if candidate.is_file():
            size_tool = str(candidate)
            break
size_tool = size_tool or shutil.which('size')
if not size_tool:
    raise SystemExit('No size tool found; pass --size-tool')
print('--size-tool\0' + size_tool, end='\0')
# The actual link target includes exactly the transitive input objects, even
# when support libraries live in a parent or sibling CMake subdirectory.
commands = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', target], text=True)
outputs = set()
for command in commands.splitlines():
    args = shlex.split(command)
    if '-o' in args:
        output = pathlib.Path(args[args.index('-o') + 1])
        output = output if output.is_absolute() else build / output
        if output.suffix in ('.o', '.obj', '.js', '.wasm'):
            outputs.add(output)
            if output.suffix == '.js':
                outputs.add(output.with_suffix('.wasm'))
    # CMake's archive command can be wrapped in `cmake -E rm ... && ar ...`.
    for arg in args:
        if arg.endswith('.a') and not arg.startswith('-'):
            path = pathlib.Path(arg)
            outputs.add(path if path.is_absolute() else build / path)
for path in sorted(outputs):
    print('--artifact\0' + str(path), end='\0')
PY
mapfile -d '' -t DISCOVERED < "$MANIFEST"
rm -f "$MANIFEST"
trap - EXIT
exec python3 "${ROOT_DIR}/scripts/bench-compile.py" \
  --build-dir "$BUILD_DIR" --target "$TARGET" --jobs "$JOBS" \
  --repeat "$REPEAT" --output "$OUTPUT" "${DISCOVERED[@]}" "${ARTIFACTS[@]}"

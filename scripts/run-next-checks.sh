#!/usr/bin/env bash
set -euo pipefail

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
  cat <<'EOF'
Usage: scripts/run-next-checks.sh

Configure, build, and run the standalone src smoke tests.

Environment overrides:
  ROOT_DIR             Repository root (default: git top-level)
  BUILD_DIR            Build directory (default: $ROOT_DIR/build-next)
  JOBS                 Build parallelism (default: 16)
  BUILD_TYPE           CMake build type (default: Debug)
  THREADS              LIGHTUSD_NEXT_ENABLE_THREAD value (default: OFF)
  ALSO_THREADS         Also build+test a THREADS=ON pass (default: 1; 0 skips)
  RUN_FUZZ             Build the libFuzzer harnesses and replay the seed
                       corpus through each when nonzero (needs clang)
  THREADS_BUILD_DIR    Build dir for that pass (default: $BUILD_DIR-threads)
  RUN_BENCH            Run opt-in memory/perf benchmarks when nonzero
  RUN_CORPUS           Run opt-in usd-wg asset corpus gate when nonzero
  USD_WG_ASSETS_DIR    usd-wg/assets checkout for RUN_CORPUS
  USDCAT_PATH           Optional OpenUSD usdcat used by usdcat roundtrip tests
  BENCH_LAZY_VERTS     bench_lazy_mem generated vertex count (default: 100000)
  BENCH_LAZY_CLONES    bench_lazy_mem clone count (default: 8)
  BENCH_LAZY_FILE      Reuse/write this USDC for bench_lazy_mem
  KEEP_BENCH_TMP       Keep generated benchmark USDC when nonzero
EOF
  exit 0
fi

ROOT_DIR="${ROOT_DIR:-$(git rev-parse --show-toplevel)}"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-next}"
JOBS="${JOBS:-16}"
BUILD_TYPE="${BUILD_TYPE:-Debug}"
RUN_BENCH="${RUN_BENCH:-0}"
RUN_CORPUS="${RUN_CORPUS:-0}"
THREADS="${THREADS:-OFF}"
ALSO_THREADS="${ALSO_THREADS:-1}"
THREADS_BUILD_DIR="${THREADS_BUILD_DIR:-$BUILD_DIR-threads}"
BENCH_LAZY_FILE_GENERATED=0

cleanup_bench_file() {
  if [ "$BENCH_LAZY_FILE_GENERATED" = "1" ] && [ "${KEEP_BENCH_TMP:-0}" = "0" ]; then
    rm -f "$BENCH_LAZY_FILE"
  fi
}
trap cleanup_bench_file EXIT

cd "$ROOT_DIR"

cmake -S "$ROOT_DIR/src" -B "$BUILD_DIR" \
  -DLIGHTUSD_NEXT_BUILD_TESTS=ON \
  -DLIGHTUSD_NEXT_ENABLE_THREAD="$THREADS" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

cmake --build "$BUILD_DIR" -j"$JOBS"

ctest --test-dir "$BUILD_DIR" --output-on-failure -L next -LE 'benchmark|corpus'

if [ "$RUN_CORPUS" != "0" ]; then
  ctest --test-dir "$BUILD_DIR" --output-on-failure -L corpus
fi

if [ "$RUN_BENCH" != "0" ]; then
  "$BUILD_DIR/bench_pcp_compose" sizes
  "$BUILD_DIR/bench_pcp_compose" compose
  "$BUILD_DIR/bench_pcp_compose" deep

  if [ -z "${BENCH_LAZY_FILE:-}" ]; then
    BENCH_LAZY_FILE="${TMPDIR:-/tmp}/lightusd-next-lazy-bench-$$.usdc"
    BENCH_LAZY_FILE_GENERATED=1
  fi
  BENCH_LAZY_VERTS="${BENCH_LAZY_VERTS:-100000}"
  BENCH_LAZY_CLONES="${BENCH_LAZY_CLONES:-8}"
  "$BUILD_DIR/bench_lazy_mem" gen "$BENCH_LAZY_FILE" "$BENCH_LAZY_VERTS"
  "$BUILD_DIR/bench_lazy_mem" eager "$BENCH_LAZY_FILE" "$BENCH_LAZY_CLONES"
  "$BUILD_DIR/bench_lazy_mem" lazy "$BENCH_LAZY_FILE" "$BENCH_LAZY_CLONES"
  "$BUILD_DIR/bench_lazy_mem" flat-lazy-owned "$BENCH_LAZY_FILE"
fi

# Second pass with threading ON.
#
# The library default is OFF, and correctly so: web/CMakeLists.txt forces it
# off because the WASM build must not require Emscripten pthreads /
# SharedArrayBuffer. The consequence is that a normal run of this script never
# COMPILES the std::thread paths -- the PropNameTable freeze/intern
# synchronisation and the parallel PCP prewarm live entirely behind
# LIGHTUSD_ENABLE_THREAD, so a change that breaks them passes every default
# check and only fails for the tools/examples that force it ON
# (lusdrender/lusdview/lusdquicklook).
#
# Set ALSO_THREADS=0 to skip (e.g. a machine without a thread library).
if [ "$ALSO_THREADS" != "0" ] && [ "$THREADS" != "ON" ]; then
  echo
  echo "=== second pass: LIGHTUSD_NEXT_ENABLE_THREAD=ON ==="
  cmake -S "$ROOT_DIR/src" -B "$THREADS_BUILD_DIR" \
    -DLIGHTUSD_NEXT_BUILD_TESTS=ON \
    -DLIGHTUSD_NEXT_ENABLE_THREAD=ON \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
  cmake --build "$THREADS_BUILD_DIR" -j"$JOBS"
  ctest --test-dir "$THREADS_BUILD_DIR" --output-on-failure -L next \
    -LE 'benchmark|corpus'
fi

# Opt-in: build the libFuzzer harnesses and replay the seed corpus through
# each one (-runs=0). This is NOT a fuzzing session -- it is a few seconds that
# keeps the harnesses compiling and proves the corpus is clean, which is what
# rots when nobody runs them. Real fuzzing is a separate, long job; see
# tests/fuzzer/README.md.
if [ "${RUN_FUZZ:-0}" != "0" ]; then
  echo
  echo "=== fuzzer harnesses: build + seed-corpus replay ==="
  FUZZ_BUILD_DIR="${FUZZ_BUILD_DIR:-$BUILD_DIR-fuzz}"
  cmake -S "$ROOT_DIR/src" -B "$FUZZ_BUILD_DIR" \
    -DLIGHTUSD_NEXT_BUILD_FUZZERS=ON \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
  cmake --build "$FUZZ_BUILD_DIR" -j"$JOBS"

  FUZZ_SEEDS="$(mktemp -d)"
  trap 'rm -rf "$FUZZ_SEEDS"' EXIT
  # next_usdc re-prepends the PXR-USDC magic, so seeds carry it stripped.
  python3 - "$ROOT_DIR" "$FUZZ_SEEDS" <<'PYEOF'
import glob, os, sys
root, out = sys.argv[1], sys.argv[2]
os.makedirs(os.path.join(out, "usdc"), exist_ok=True)
os.makedirs(os.path.join(out, "usda"), exist_ok=True)
for f in glob.glob(os.path.join(root, "tests/usdc/*.usdc")):
    d = open(f, "rb").read()
    if d[:8] == b"PXR-USDC":
        open(os.path.join(out, "usdc", os.path.basename(f)), "wb").write(d[8:])
for f in glob.glob(os.path.join(root, "tests/usda/*.usda"))[:400]:
    open(os.path.join(out, "usda", os.path.basename(f)), "wb").write(
        open(f, "rb").read())
PYEOF
  "$FUZZ_BUILD_DIR/fuzz_next_usdc" -runs=0 "$FUZZ_SEEDS/usdc"
  "$FUZZ_BUILD_DIR/fuzz_next_usda" -runs=0 "$FUZZ_SEEDS/usda"
  "$FUZZ_BUILD_DIR/fuzz_next_compose" -runs=0 \
    "$ROOT_DIR/tests/fuzzer/next_compose_seeds"
  "$FUZZ_BUILD_DIR/fuzz_next_roundtrip" -runs=0 \
    "$FUZZ_SEEDS/usda"
fi

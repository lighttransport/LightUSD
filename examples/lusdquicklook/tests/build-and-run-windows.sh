#!/usr/bin/env bash
# Cross-build lusdquicklook for Windows and smoke-test it under Wine.
#
# Prefers llvm-mingw (set LLVM_MINGW_DIR); falls back to a system mingw-w64 GCC
# cross toolchain. Neither is required to be present -- the script says what is
# missing and exits 0 so it can sit in a pipeline without failing hosts that
# cannot cross-compile.
#
# usage: build-and-run-windows.sh [build-dir]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BUILD="${1:-$ROOT/build-mingw-quicklook}"

note() { echo "[win-cross] $*"; }
skip() { echo "[win-cross] SKIP: $*"; exit 0; }

# ---- Pick a toolchain -------------------------------------------------------
TOOLCHAIN=""
GCC_WORKAROUND=""
STATIC_RUNTIME=""
if [ -n "${LLVM_MINGW_DIR:-}" ] && \
   [ -x "$LLVM_MINGW_DIR/bin/x86_64-w64-mingw32-clang++" ]; then
  TOOLCHAIN="$ROOT/cmake/llvm-mingw-cross.cmake"
  note "using llvm-mingw at $LLVM_MINGW_DIR"
elif command -v x86_64-w64-mingw32-g++-posix >/dev/null 2>&1 || \
     command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
  TOOLCHAIN="$ROOT/cmake/mingw64-cross.cmake"
  note "using system mingw-w64 GCC"
  # mingw-w64 GCC reports a -Wmaybe-uninitialized false positive inside
  # src/legacy/usdMtlx.cc (via value-types.hh placement new), and the library builds
  # with -Werror. That is pre-existing library code, unrelated to
  # lusdquicklook, so downgrade just that warning rather than patching it here.
  GCC_WORKAROUND="-Wno-error=maybe-uninitialized"
  # Static runtime: a mingw-GCC build otherwise imports libstdc++-6.dll,
  # libgcc_s_seh-1.dll and libwinpthread-1.dll, so the .exe will not start
  # unless those DLLs sit beside it. A single self-contained binary is what
  # "portable" should mean here, and it is what makes the Wine run work.
  STATIC_RUNTIME="-static-libgcc -static-libstdc++ -static"
else
  skip "no Windows cross toolchain found (set LLVM_MINGW_DIR or install mingw-w64)"
fi

# ---- Configure + build ------------------------------------------------------
note "configuring in $BUILD"
cmake -S "$ROOT" -B "$BUILD" \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
  -DCMAKE_BUILD_TYPE=Release \
  -DLIGHTUSD_BUILD_EXAMPLES=ON \
  -DLIGHTUSD_WITH_TYDRA=ON \
  -DLIGHTUSD_BUILD_QUICKLOOK=ON \
  -DLIGHTUSD_BUILD_GUI_VIEWER=OFF \
  -DLIGHTUSD_BUILD_TESTS=OFF \
  -DLIGHTUSD_BUILD_TOOLS=OFF \
  ${GCC_WORKAROUND:+-DCMAKE_CXX_FLAGS="$GCC_WORKAROUND"} \
  ${STATIC_RUNTIME:+-DCMAKE_EXE_LINKER_FLAGS="$STATIC_RUNTIME"} \
  ${EXTRA_CMAKE_ARGS:-} >/dev/null

note "building"
cmake --build "$BUILD" --target lusdquicklook -j"$(nproc)"

EXE="$BUILD/lusdquicklook.exe"
[ -f "$EXE" ] || EXE="$BUILD/lusdquicklook"
[ -f "$EXE" ] || { echo "[win-cross] FAIL: no binary produced" >&2; exit 1; }
note "built $EXE"

# ---- Run under Wine ---------------------------------------------------------
if ! command -v wine >/dev/null 2>&1; then
  skip "wine not installed; built the binary but cannot run it"
fi

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
OUT_PROBE_ERR="$OUT/probe.err"

export WINEDEBUG="${WINEDEBUG:--all}"

# Probe that Wine can run ANY Windows binary before blaming ours. An incomplete
# install (for example a 64-bit-only wine missing the wine32 loader) fails every
# exe with the same exit code, which would otherwise look like our bug.
if ! wine cmd /c exit 0 >/dev/null 2>&1; then
  skip "wine cannot run Windows binaries here (incomplete install?); the .exe built fine"
fi

# `wine cmd` can succeed while our binary still fails to load (a missing
# runtime DLL, a wow64 gap). Probe the actual binary with --help before running
# the real workload, so a load failure is reported as such.
if ! wine "$EXE" --help >/dev/null 2>"$OUT_PROBE_ERR"; then
  echo "[win-cross] FAIL: the binary does not start under wine" >&2
  grep -v "^[0-9a-f]*:" "$OUT_PROBE_ERR" 2>/dev/null | head -5 >&2 || true
  x86_64-w64-mingw32-objdump -p "$EXE" 2>/dev/null | grep "DLL Name" | sort -u >&2 || true
  exit 1
fi

# The GL backend needs a Windows GL driver, which Wine only sometimes provides;
# force the CPU renderer so this tests our code, not Wine's graphics stack.
note "running under wine"
if ! wine "$EXE" "$ROOT/models/cube-previewsurface.usda" \
     --backend cpu --screenshot "$OUT/win.png" --size 320x240 --frames 4 \
     >"$OUT/stdout.txt" 2>"$OUT/stderr.txt"; then
  echo "[win-cross] FAIL: wine run exited non-zero" >&2
  cat "$OUT/stderr.txt" >&2
  exit 1
fi

[ -s "$OUT/win.png" ] || { echo "[win-cross] FAIL: no PNG written" >&2; exit 1; }
head -c 8 "$OUT/win.png" | od -An -tx1 | tr -d ' \n' | \
  grep -qi '^89504e470d0a1a0a$' \
  || { echo "[win-cross] FAIL: output is not a PNG" >&2; exit 1; }

note "PASS: wine produced a valid PNG ($(stat -c%s "$OUT/win.png") bytes)"

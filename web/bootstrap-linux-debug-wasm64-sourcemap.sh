#!/bin/bash
# Build LightUSD WASM with source maps and enhanced debugging
# This enables C++ source line visibility in browser DevTools on abort/crash

rm -rf build_debug_sourcemap_64
mkdir build_debug_sourcemap_64

emcmake cmake -DLIGHTUSD_WASM_PRODUCT=legacy \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLIGHTUSD_WASM_DEBUG=ON \
  -DLIGHTUSD_WASM64=1 \
  -DCMAKE_VERBOSE_MAKEFILE=1 \
  -Bbuild_debug_sourcemap_64

echo ""
echo "Build configured with source maps enabled."
echo "To build, run:"
echo "  cd build_debug_sourcemap_64 && make -j$(nproc)"
echo ""
echo "Output will include:"
echo "  - lightusd.wasm.map (source map file)"
echo "  - Enhanced stack traces with C++ source lines"
echo "  - Additional runtime checks (SAFE_HEAP, STACK_OVERFLOW_CHECK)"
echo ""

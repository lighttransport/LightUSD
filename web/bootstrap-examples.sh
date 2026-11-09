#!/bin/bash

# LightUSD Web Build Examples
# This script demonstrates different build configurations with various memory limits

echo "LightUSD Web Build Examples"
echo "=========================="
echo

# Clean previous builds
rm -rf build

echo "1. Standard WASM32 build (2GB memory limit default)"
echo "---------------------------------------------------"
mkdir build-wasm32
emcmake cmake -DLIGHTUSD_WASM_PRODUCT=legacy -DCMAKE_BUILD_TYPE=MinSizeRel -DLIGHTUSD_WASM64=OFF -Bbuild-wasm32
echo "Build command: emcmake cmake -DCMAKE_BUILD_TYPE=MinSizeRel -DLIGHTUSD_WASM64=OFF -Bbuild-wasm32"
echo "Memory default: 2GB (2048 MB)"
echo

echo "2. WASM64/MEMORY64 build (8GB memory limit default)"
echo "---------------------------------------------------"
mkdir build-wasm64
emcmake cmake -DLIGHTUSD_WASM_PRODUCT=legacy -DCMAKE_BUILD_TYPE=MinSizeRel -DLIGHTUSD_WASM64=ON -Bbuild-wasm64
echo "Build command: emcmake cmake -DCMAKE_BUILD_TYPE=MinSizeRel -DLIGHTUSD_WASM64=ON -Bbuild-wasm64"
echo "Memory default: 8GB (8192 MB)"
echo

echo "To build, run:"
echo "  make -C build-wasm32  # For WASM32 build"
echo "  make -C build-wasm64  # For WASM64 build"
echo

echo "Note: WASM64/MEMORY64 requires browsers with MEMORY64 support"
echo "(Chrome 109+, Firefox 102+ with flags enabled)"
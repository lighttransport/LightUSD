# Core-only WASM32 build: core + Tydra module
# (-DLIGHTUSD_WASM_PRODUCT=core), no legacy loader. Output:
# js/src/lightusd/lightusd.js/.wasm.
# Build with: `ninja -C build_core_only` (or `cmake --build build_core_only`).
rm -rf build_core_only
mkdir build_core_only

emcmake cmake -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel -DLIGHTUSD_WASM_PRODUCT=core -DCMAKE_VERBOSE_MAKEFILE=1 -Bbuild_core_only

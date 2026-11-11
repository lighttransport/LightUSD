# Legacy-only WASM32 build: the lightusd module WITHOUT the core
# (-DLIGHTUSD_WASM_PRODUCT=legacy). Drops the flatten* low-memory flatten
# pipeline and the RenderStream streaming API; the classic loadFromBinary/
# Tydra RenderScene path is unchanged. Output: js/src/lightusd/
# lightusd.js/.wasm.
# Build with: `ninja -C build_legacy_only` (or `cmake --build build_legacy_only`).
rm -rf build_legacy_only
mkdir build_legacy_only

emcmake cmake -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel -DLIGHTUSD_WASM_PRODUCT=legacy -DCMAKE_VERBOSE_MAKEFILE=1 -Bbuild_legacy_only

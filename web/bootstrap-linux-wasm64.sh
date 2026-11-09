# Default WASM64 / MEMORY64 build: Ninja generator + MinSizeRel.
#
# MinSizeRel applies emscripten's link-time -Oz (and drops runtime assertions),
# which is what keeps the .wasm small (~5MB vs ~13MB for a plain Release link).
# Build with: `ninja -C build_64` (or `cmake --build build_64`).
# For a speed-over-size build instead, use bootstrap-linux-wasm64-release.sh.
builddir=build_64
rm -rf ${builddir}
mkdir ${builddir}

emcmake cmake -DLIGHTUSD_WASM_PRODUCT=legacy -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_VERBOSE_MAKEFILE=1 -DLIGHTUSD_WASM64=1 -B${builddir}

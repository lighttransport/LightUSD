# Default WASM32 build: Ninja generator + MinSizeRel.
# Builds the core module (lightusd.js/.wasm): the core + Tydra, with the
# nextFlatten* lazy pipeline and RenderStream. Not the legacy compatibility
# module; use bootstrap-linux-legacy-only.sh for that (or, for the legacy
# product, bootstrap-linux-legacy-only.sh, which is the former default).
#
# MinSizeRel applies emscripten's link-time -Oz (and drops runtime assertions),
# which is what keeps the .wasm small (~5MB vs ~13MB for a plain Release link).
# Build with: `ninja -C build` (or `cmake --build build`).
# For a speed-over-size build instead, use bootstrap-linux-release.sh.
rm -rf build
mkdir build

emcmake cmake -DLIGHTUSD_WASM_PRODUCT=core -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_VERBOSE_MAKEFILE=1 -Bbuild

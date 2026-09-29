# Experimental "speed over size" build (wasm64). Default is
# bootstrap-linux-wasm64.sh (MinSizeRel). Use this when CPU-bound USD parsing
# dominates and the larger .wasm is acceptable — drops link-time -Oz and
# runtime assertions.
builddir=build_64
rm -rf ${builddir}
mkdir ${builddir}

emcmake cmake -DLIGHTUSD_WASM_PRODUCT=legacy -DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE=1 -DLIGHTUSD_WASM64=1 -B${builddir}

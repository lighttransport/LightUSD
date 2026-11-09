# Experimental "speed over size" build (wasm32). Default is bootstrap-linux.sh
# (MinSizeRel). Use this when CPU-bound USD parsing dominates and the larger
# .wasm is acceptable — drops link-time -Oz and runtime assertions.
builddir=build_release
rm -rf ${builddir}
mkdir ${builddir}

emcmake cmake -DLIGHTUSD_WASM_PRODUCT=legacy -DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE=1 -B${builddir}

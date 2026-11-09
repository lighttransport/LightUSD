builddir=build_64
rm -rf ${builddir}
mkdir ${builddir}

emcmake cmake -DLIGHTUSD_WASM_PRODUCT=legacy -DCMAKE_BUILD_TYPE=MinSizeRel -DLIGHTUSD_WASM_COROUTINE=On -DCMAKE_VERBOSE_MAKEFILE=1 -DLIGHTUSD_WASM64=1 -B${builddir}

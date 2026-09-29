builddir=build_64_ninja
rm -rf ${builddir}
mkdir ${builddir}

emcmake cmake -DLIGHTUSD_WASM_PRODUCT=legacy -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_VERBOSE_MAKEFILE=1 -DLIGHTUSD_WASM64=1 -B${builddir}

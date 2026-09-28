rm -rf build_ninja
mkdir build_ninja

emcmake cmake -DLIGHTUSD_WASM_PRODUCT=legacy -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_VERBOSE_MAKEFILE=1 -Bbuild_ninja

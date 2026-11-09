rm -rf build_debug
mkdir build_debug

emcmake cmake -DLIGHTUSD_WASM_PRODUCT=legacy -DCMAKE_BUILD_TYPE=Debug -DCMAKE_VERBOSE_MAKEFILE=1 -Bbuild_debug

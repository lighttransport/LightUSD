rm -rf build
mkdir build

emcmake cmake -DLIGHTUSD_WASM_PRODUCT=legacy -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_VERBOSE_MAKEFILE=1 -DLIGHTUSD_WASM64=1 -Bbuild

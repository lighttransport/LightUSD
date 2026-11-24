curdir=`pwd`

builddir=${curdir}/build_asan

rm -rf ${builddir}
mkdir ${builddir}

# with lld linker
#  -DCMAKE_TOOLCHAIN_FILE=cmake/lld-linux.toolchain.cmake

# Honor CXX/CC from the environment (CI passes CXX=g++ CC=gcc — clang's
# Ubuntu compiler-rt layout doesn't match where libclang-rt-XX-dev
# installs its aarch64 runtime, so -fsanitize=address fails to link).
# Falls back to clang if nothing was set.
: "${CXX:=clang++}"
: "${CC:=clang}"

cd ${builddir} && CXX="${CXX}" CC="${CC}" cmake \
  -DSANITIZE_ADDRESS=1 \
  -DLIGHTUSD_NO_WERROR=On \
  -DCMAKE_VERBOSE_MAKEFILE=1 \
  ..

# -DLIGHTUSD_NO_WERROR=On: gcc's static analyzer under ASan
# instrumentation generates false-positive -Warray-bounds /
# -Wstringop-truncation warnings on vendored stb_image_write.h and
# strncpy() use in crate-writer.cc that don't fire in non-ASan builds.
# This is an instrumented debug build, not production validation, so
# turning off -Werror is the right tradeoff.


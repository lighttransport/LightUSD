curdir=`pwd`

# For Ubuntu, install 32bit libc, libstdc++, etc libraries before the build.
# `gcc-multilib` and `g++-multilib` is the easier solution
# sudo apt install gcc-multilib g++-multilib


builddir=${curdir}/build_m32

rm -rf ${builddir}
mkdir ${builddir}


# Use gcc, not clang: Ubuntu does not ship clang's i386 ASan runtime
# (no libclang-rt-XX-dev:i386 package). gcc's libasan ships with
# gcc-multilib and works for both 64-bit hosts compiling -m32.
cd ${builddir} && CC=gcc CXX=g++ cmake \
  -DCMAKE_TOOLCHAIN_FILE=cmake/linux_i386.toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Debug \
  -DSANITIZE_ADDRESS=1 \
  -DLIGHTUSD_NO_WERROR=On \
  -DCMAKE_VERBOSE_MAKEFILE=1 \
  -DLIGHTUSD_NATIVE_PRODUCT=legacy \
  ..

# -DLIGHTUSD_NO_WERROR=On: same reason as the asan script — gcc's
# warnings under ASan instrumentation are noisier than the regular
# -Wall build, and this i386+ASan job is a debug check, not a
# production -Werror gate.

curdir=`pwd`

builddir=${curdir}/build

rm -rf ${builddir}
mkdir ${builddir}

CC=clang
CXX=clang++

# with lld linker
#  -DCMAKE_TOOLCHAIN_FILE=cmake/lld-linux.toolchain.cmake

# Use CC/CXX environment variables if set, otherwise let CMake detect
if [ -n "${CC}" ] && [ -n "${CXX}" ]; then
  cd ${builddir} && CC=${CC} CXX=${CXX} cmake -G Ninja \
    -DCMAKE_VERBOSE_MAKEFILE=1 \
    ..
else
  cd ${builddir} && cmake -G Ninja \
    -DCMAKE_VERBOSE_MAKEFILE=1 \
    ..
fi


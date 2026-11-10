curdir=`pwd`

builddir=${curdir}/build_release

rm -rf ${builddir}
mkdir ${builddir}

# with lld linker
#  -DCMAKE_TOOLCHAIN_FILE=cmake/lld-linux.toolchain.cmake 

cd ${builddir} && CXX=clang++-21 CC=clang-21 cmake \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_VERBOSE_MAKEFILE=1 \
  ..


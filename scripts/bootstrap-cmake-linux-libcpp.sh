curdir=`pwd`

builddir=${curdir}/build_libcpp

rm -rf ${builddir}
mkdir ${builddir}

# with lld linker
#  -DCMAKE_TOOLCHAIN_FILE=cmake/lld-linux.toolchain.cmake 

cd ${builddir} && CXX=clang++-18 CC=clang-18 cmake \
  -DCMAKE_CXX_FLAGS="-stdlib=libc++" \
  -DCMAKE_VERBOSE_MAKEFILE=1 \
  ..


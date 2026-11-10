curdir=`pwd`

builddir=${curdir}/build-cross

rm -rf ${builddir}
mkdir ${builddir}

cd ${builddir} && cmake \
  -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.toolchain \
  -DCMAKE_VERBOSE_MAKEFILE=1 \
  -DLIGHTUSD_WITH_EXR=1 \
  -DLIGHTUSD_WITH_TIFF=1 \
  -DLIGHTUSD_BUILD_TESTS=Off \
  -DLIGHTUSD_BUILD_EXAMPLES=Off \
  ..


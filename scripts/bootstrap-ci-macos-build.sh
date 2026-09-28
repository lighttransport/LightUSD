curdir=`pwd`

builddir=${curdir}/build

rm -rf ${builddir}
mkdir ${builddir}

#  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
cd ${builddir} && CXX=clang++ CC=clang cmake \
  -DCMAKE_VERBOSE_MAKEFILE=1 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DLIGHTUSD_WITH_EXR=1 \
  -DLIGHTUSD_BUILD_BENCHMARKS=1 \
  -DSANITIZE_ADDRESS=0 \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DLIGHTUSD_NATIVE_PRODUCT=legacy \
  ..

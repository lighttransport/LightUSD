curdir=`pwd`

builddir=${curdir}/build_ubsan

rm -rf ${builddir}
mkdir ${builddir}

cd ${builddir} && CXX=clang++ CC=clang cmake \
  -DSANITIZE_UNDEFINED=1 \
  -DCMAKE_VERBOSE_MAKEFILE=1 \
  ..


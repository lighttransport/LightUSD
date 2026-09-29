curdir=`pwd`

builddir=${curdir}/build

rm -rf ${builddir}
mkdir ${builddir}


cd ${builddir} && cmake \
  -DSANITIZE_ADDRESS=1 \
  -DCMAKE_VERBOSE_MAKEFILE=1 \
  -DLIGHTUSD_NATIVE_PRODUCT=legacy \
  ..


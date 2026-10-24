// SPDX-License-Identifier: Apache-2.0

#define TINYVDB_IO_IMPLEMENTATION
/* BLOSC-zstd decoding when the build provides src/external/zstd.c. */
#if defined(LIGHTUSD_TINYVDB_ZSTD)
#define TVDB_USE_ZSTD
#endif
#include "tinyvdb_io.h"

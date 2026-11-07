# tptex vendor import (pristine snapshot)

Upstream: tinyexr `release` branch, `tools/ptex` (https://github.com/syoyo/tinyexr).
Imported 2026-11-07 at tinyexr commit **`b3b5dcf`** (local branch `release`, unpushed
when imported; re-sync once pushed).

A pure-C11 port of Disney's Ptex (reader, writer, half, utils, filters, plus the
seam-aware `tptex_sample` ported from LightUSD's former `src/ptex/lptex-sample`).
Differentially tested against upstream OpenPtex 2.4.3 in tinyexr (`make ptex-compare`:
half exhaustive, utils, reader, writer, filters).

Keep it pristine: fix bugs upstream and re-sync. Imported: `include/`, `src/`, `test/`,
`LICENSE`, `NOTICE`, `README.md`. Skipped: `compare/` (fetches and builds upstream C++),
`ptex.mk` (tinyexr Makefile rules).

## Build here

`lightusd_ptex_objects` (cmake/lightusd-render-support.cmake) compiles the reader,
half, utils, status, allocator, inflate (against the in-tree `src/external/miniz`),
memory input and `tptex_sample`, plus `tptex_file.c` when `LIGHTUSD_PTEX_ENABLE_FILEIO`
is on. The writer and filters are not part of the product objects (the renderers do
not use them); the `unit-test-lightusd` ptx tests build them from this directory.
`src/ptx-loader.{hh,cc}` is the thin C++ adapter over this library.

## lightusd-local patches

None.

## Re-sync

`rsync -a --delete --exclude compare --exclude ptex.mk <tinyexr>/tools/ptex/{include,src,test,LICENSE,NOTICE,README.md} src/external/textools/ptex/`,
update the commit above, then run
`ctest --test-dir build_ninja -R 'ptx|ptex' --output-on-failure` (via `scripts/xctest.sh`).

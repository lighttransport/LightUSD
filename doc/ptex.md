# Ptex in LightUSD

LightUSD reads Ptex (`.ptx`) textures with **tptex**, a pure C11 port of Disney's
Ptex that lives in tinyexr (`tools/ptex`) and is vendored here, unmodified, at
`src/external/textools/ptex` (see its `README.lightusd.md` for the synced commit).
tptex is differentially tested against upstream OpenPtex 2.4.3 in tinyexr
(`make ptex-compare`: half conversion exhaustively, utils, reader, writer and
filters). OpenPtex is also used here as an independent oracle for the renderers
(`tests/verification/repros/ptex-*-compare.cc`, `tool-lusdrender-vkr-ptex-openptex`).

| File | Contents |
|---|---|
| `src/external/textools/ptex/include/tptex.h` | Reader API (memory or file input), face info, metadata |
| `.../tptex_sample.{h,c}` | Stateless seam-aware sampling math (ported from the former `lptex-sample`) |
| `.../tptex_write.h`, `tptex_filter.h` | Writer and PtexFilter equivalents (not linked into the renderers) |
| `src/ptx-loader.{hh,cc}` | C++ `lightusd::ptx::Reader` adapter (API unchanged) |
| `examples/lusdview/cpu/ptex-sample.hh` | `CpuPtexSampler`: LRU page cache + mutex over `tptex_sample` |

## What tptex covers

Everything upstream Ptex reads: format version 1; u8/u16/half/float data; quad and
triangle meshes; constant, zip, difference-zip and tiled faces; stored reduction
levels and on-the-fly reductions (upstream's alternating-axis rule, triangle
reduction, half/float included); the average-colour block; metadata (small and large,
lazily loaded); edit blocks; border and edge-filter modes. Big-endian hosts work
(explicit little-endian parsing and byte swapping; upstream refuses them).

`lightusd::ptx::Reader::ReadFace(face, level, ...)` returns the face at
`(ures - level, vres - level)` texels, each axis clamped at 1. Versus the former
reader this changes results only where the old code generated its own reductions
for non-conforming data: reductions now follow upstream exactly (for example a fully
reduced 1×1 face is the stored average colour).

## Guarantees

- No global mutable state, atomics, `volatile`, threads or `alloca`. Memory comes from an
  optional allocator. `stdio`/POSIX I/O is confined to `tptex_file.c`.
- Every size, offset and count read from a file is overflow-checked and bounded by
  `tptex_limits` and the input size. Corrupt files fail with a status, never undefined
  behaviour (ASan/UBSan fuzzing in tinyexr and the hostile-mutation phase of `cmp_reader`).
- The adapter opens textures `TPTEX_OPEN_NO_LAZY`: every level header and all metadata
  are loaded at open and a malformed level or metadata block only fails the reads that
  need it. After that `ReadFace` does not mutate the texture, so it may be called
  concurrently on one `Reader` (the memory and `pread` inputs are stateless).
- Floating-point code is built with `-ffp-contract=off` so results do not depend on FMA
  fusion across compilers or targets.

## Sampling (`tptex_sample.h`)

Pure functions over a caller page callback
`int fetch(void* user, uint32_t face, uint32_t mip, tptex_image* page)`; the page is
borrowed only until the next fetch. `tptex_cross_edge`/`tptex_resolve_tap` (adjacency
remap with the `PtexSeparableFilter::applyAcrossEdge` rotation plus a corner hop; subface
adjacency is not followed), `tptex_sample_rgb`/`_channel` (seam-aware bilinear),
`tptex_compute_uv_footprint` and `tptex_sample_footprint_*` (anisotropic: minor-axis mip,
up to 8 tent-weighted taps along the major axis), `tptex_choose_mip`,
`tptex_srgb_to_linear`, `tptex_sample_half_to_float`. Cache policy stays with the caller.
These are behaviour-identical to the previous `lptex_*` functions, so renders are
unchanged. The full upstream filters (`tptex_filter.h`) are available but not yet used by
the viewers.

## Build

The default renderer backend is the vendored tinyexr `tptex` implementation.
WDAS/OpenPtex sources and libraries are **optional verification dependencies**;
normal builds and Ptex rendering do not discover, download or link them.
The Vulkan oracle runs only when `LIGHTUSD_PTEX_INCLUDE` explicitly points to
OpenPtex >=2.4 sources (`src/ptex`). The CPU element oracle requires explicit
`LIGHTUSD_PTEX_SOURCE` (OpenPtex root) and `LIGHTUSD_PTEX_LIBRARY` (directory
containing `libPtex.so`). Without these settings, the oracle tests skip (77);
the vendored reader/sampler tests still run.

`cmake/lightusd-ptex.cmake` lists the tptex sources the renderers need (reader, half,
utils, status, allocator, memory input, inflate against the in-tree miniz, sampler) and
`cmake/lightusd-render-support.cmake` builds them into `lightusd_ptex_objects`, embedded in
the legacy and next/tydra archives. `LIGHTUSD_PTEX_ENABLE_FILEIO` adds `tptex_file.c`
(default OFF for Emscripten/WASI, ON elsewhere); when OFF, `TPTEX_NO_FILEIO` is defined and
`Reader::OpenFile` uses only the mmap path.

## Tests

- `unit-test-lightusd ptx_reader_*`. `ptx_reader_legacy_differential_test` compares the
  tptex-backed `lightusd::ptx::Reader` with a frozen copy of the pre-rewrite reader
  (`tests/unit/ptx-legacy-reader.inc`) on the synthetic fixtures, 400 randomized
  containers, corruptions of each, and the lmetalview fixture. Hand-built and real files
  must match at every level, the randomized corpus at level 0; corrupted inputs are
  informational because tptex follows upstream Ptex where the legacy reader was more
  permissive. Corrupted headers declaring face, constant or metadata tables above
  the harness's 16 MiB open-table budget are counted separately as budget skips;
  the frozen reference cannot safely open them under i386 ASan. Valid corpus
  files are not filtered, and sanitizer allocation/error settings remain unchanged.
  Env: `LIGHTUSD_PTX_DIFF_FILES=a.ptx:b.ptx` adds real files,
  `LIGHTUSD_PTX_DUMP_CORPUS=dir` dumps the corpus (fuzzer seeds).
- `fuzz_tptex` (`-DLIGHTUSD_BUILD_FUZZERS=ON`, Clang; `tests/fuzzer/tptex_fuzzmain.c`):
  libFuzzer with ASan/UBSan over open (lazy and eager), face decode at stored and reduced
  resolutions, metadata, seam-aware sampling and all filters. The legacy `src/ptex` core and
  `fuzz_lptex` are gone; the frozen reference reader lives in `tests/unit/ptx-legacy-reader.inc`.
- The lusdview ctests `lusdview-cpu-ptex-element`, `lusdview-cpu-ptex-routing`,
  `lusdview-ptex-camera-demand`, `lusdview_ptex_atlas_test`, and the OpenPtex oracle
  `tool-lusdrender-vkr-ptex-openptex`
  (`doc/validation/island-ptex-release-control-2026-11-03.md`).
- tptex's own suite (unit tests, upstream differential, fuzzing) runs in tinyexr; its unit
  tests are also vendored under `src/external/textools/ptex/test`.

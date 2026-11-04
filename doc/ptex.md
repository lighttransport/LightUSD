# Ptex in LightUSD

LightUSD reads Ptex (`.ptx`) textures with its own small C11 library in
`src/ptex/`. It does not link OpenPtex. OpenPtex is used only as an
independent test oracle (`tests/verification/repros/ptex-*-compare.cc`).

| File | Contents |
|---|---|
| `src/ptex/lptex.h`, `lptex.c` | Container reader and face decoder (memory only) |
| `src/ptex/lptex-sample.h`, `lptex-sample.c` | Stateless sampling math |
| `src/ptex/lptex-file.c` | `lptex_open_file()`; built only with file IO |
| `src/ptx-loader.{hh,cc}` | C++ `lightusd::ptx::Reader` wrapper (unchanged API) |
| `examples/lusdview/cpu/ptex-sample.hh` | `CpuPtexSampler`: LRU page cache + mutex over `lptex-sample` |

## Reader API (`lptex.h`)

```c
lptex_reader* r = NULL;
lptex_status st = lptex_open_memory(bytes, size, NULL /*options*/, &r);
const lptex_info* info = lptex_reader_info(r);   /* type, channels, levels, faces, face_info[] */
lptex_image img;
st = lptex_read_face(r, face, level, max_bytes, &img);  /* or lptex_read_face_into(dst) */
lptex_image_free(r, &img);
lptex_close(r);
```

- The reader borrows `bytes`. `lptex_open_file()` owns them instead: it maps
  the file with POSIX `mmap`, or reads it with `read`/`fread` when mapping is
  not possible.
- `lptex_options.allocator` takes `{alloc, free, user}`. Leaving it NULL means
  `malloc`/`free`. All reader state and decode scratch come from it.
- Errors are `lptex_status` codes. `lptex_status_str()` returns a static
  message (the same text the old C++ reader produced), so no string is ever
  allocated.
- Decoded images are channel-interleaved, with rows v-major starting at v = 0.
  This matches OpenPtex `getData`.

Covered: format version 1; u8/u16/half/float data; constant faces; zip and
difference-zip faces; tiled faces; per-level face ordering; generated
(reduced) integer mips for levels that a writer stored only partially, using
OpenPtex's axis-alternating truncating reduction. Not covered: metadata
(skipped), edit blocks, and dynamic reduction of half/float data or triangle
meshes. Those cases return `LPTEX_E_REDUCTION_UNSUPPORTED`.

## Guarantees

- No global or static mutable state, no exceptions, no `volatile` and no
  atomics. The reader is immutable after open, so `lptex_read_face*()` is
  reentrant and safe to call concurrently on one reader without locks.
- Every size, offset and count read from the file is checked with
  overflow-safe add/mul before use. Multi-byte fields are read with
  little-endian byte loads (no struct punning). zlib blocks must inflate to
  exactly the expected size. `max_bytes` bounds every decoded face, including
  the parent levels used for generated mips (recursion depth is at most 32).
  A level whose face count is larger than the face table is rejected.
- Reduction-level faces are located by rank under the exact former
  `std::sort` comparator: `min(w,h) >> level` descending, then face index
  ascending. That comparator is a strict total order, so the rank equals the
  sorted position. It costs O(faces) per read and needs no allocation.
- Sampling functions guard float-to-int conversions (NaN and inf are defined
  behaviour). For finite inputs the `std::min/max/clamp` semantics are kept
  bit-for-bit.
- The code is C11 and `-pedantic` clean on GCC 15, GCC 14, Clang 21
  (`-Weverything` minus `-Wpadded`, `-Wunsafe-buffer-usage`,
  `-Wdeclaration-after-statement` and `-Wswitch-default`), emcc, and 32-bit
  (`-m32 -Wconversion`).

C++ users keep `lightusd::ptx::Reader` (`OpenFile`, `OpenMemory`, `ReadFace`)
and `lusdview::CpuPtexSampler`. Only private members changed.

## Sampling (`lptex-sample.h`)

These functions are pure, over a caller page callback
`int fetch(void* user, uint32_t face, uint32_t mip, lptex_image* page)`.
The page is borrowed only until the next fetch.

- `lptex_cross_edge()` and `lptex_resolve_tap()`: adjacency remap with the
  `PtexSeparableFilter::applyAcrossEdge` rotation, plus a second hop for
  corners. Subface adjacency is not followed.
- `lptex_sample_rgb()` and `lptex_sample_channel()`: bilinear sampling where
  each tap follows the seam into the neighbouring face.
- `lptex_compute_uv_footprint()` and `lptex_sample_footprint_*()`: the
  anisotropic footprint. The mip comes from the minor axis, and up to 8
  tent-weighted taps are taken along the major axis.
- `lptex_choose_mip()`, `lptex_srgb_to_linear()` and `lptex_half_to_float()`.

The cache policy stays outside the C core. `CpuPtexSampler` keeps its LRU,
its mutex and its statistics.

## Build

`cmake/lightusd-render-support.cmake` adds `lptex.c` and `lptex-sample.c` to
`lightusd_ptex_objects` (C11, no extensions). That object library is embedded
in both the legacy and the next/tydra archives. `LIGHTUSD_PTEX_ENABLE_FILEIO`
adds `lptex-file.c`. It defaults to OFF for Emscripten/WASI and ON elsewhere.
When it is OFF, `LPTEX_NO_FILEIO` is defined and `Reader::OpenFile` uses only
the mmap path.

## Tests

- `unit-test-lightusd ptx_reader_*`: the existing assertions, plus
  `ptx_reader_legacy_differential_test`. That test compares lptex and the C++
  wrapper against a frozen copy of the pre-rewrite reader
  (`tests/unit/ptx-legacy-reader.inc`). It checks open status and message,
  info, and every face × level × budget for bytes and status. The inputs are
  the synthetic fixtures, 400 randomized multi-face containers (all types,
  encodings, tiles, partial reduction levels), 24 corruptions of each, and the
  lmetalview fixture.
  - `LIGHTUSD_PTX_DIFF_FILES=a.ptx:b.ptx` adds real files. Faces are sampled
    when a file has more than about 1500.
  - `LIGHTUSD_PTX_DUMP_CORPUS=dir` writes the generated corpus, for example
    as fuzzer seeds.
- `fuzz_lptex` (`-DLIGHTUSD_NEXT_BUILD_FUZZERS=ON`, Clang): libFuzzer with
  ASan/UBSan over open, decode with a 1 MiB budget, and seam-aware sampling.
- The lusdview ctests `lusdview-cpu-ptex-element`, `lusdview-cpu-ptex-routing`,
  `lusdview-ptex-camera-demand` and `lusdview_ptex_atlas_test`, and the
  OpenPtex oracle described in
  `doc/validation/island-ptex-release-control-2026-11-03.md`.

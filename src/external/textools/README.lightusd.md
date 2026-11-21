# textools Vendor Import (pristine snapshot)

Upstream: tinyexr `release` branch, `tools/` subtree
(local worktree: `<tinyexr-worktree>`, https://github.com/syoyo/tinyexr).

Four pure-C11 texture libraries imported as a **pristine snapshot** (not a
fork, not a submodule). Initial import at upstream commit
`8b89eea948b221321df19773968d38735a611257` on 2026-07-05.

Re-synced 2026-11-04 from upstream **`release`** commit `96dabe5`. This brings
texcomp's two faster BC7 speed profiles (`3cf96ad` fast, `88b6d7d` fastest) and
the `7ac2417` input-validation hardening across texcomp. No new library files
(only the skipped CLIs were added upstream).

The previous 2026-07-15 re-sync was from upstream **`release`** commit `1b10661`
(PR #259 merged). This adds Zstd-supercompressed KTX2 writing through callback-based
`tp_ktx2_write_uni_zstd`, strict shared validation for raw/Zstd uni mip chains,
and an explicit standard ASTC KTX2 carrier (`TP_UNI_ASTC_KTX2`). The default
carrier remains TinyEXR-private and now uses an UNSPECIFIED DFD model so genuine
Basis UASTC is never silently misclassified as texcomp uni. The upstream PR also
adds an optional standalone Three.js KTX2Loader render/readback test; lightusd
has an equivalent CMake-registered browser gate in
`web/js/tests/texture-compression-three-ktx2.mjs`.

The previous 2026-07-11 sync from `4066447` (PR #258) brought the KTX2 reader /
transcode-on-load API (including Zstd read via `tp_ktx2_read_zstd`, crafted-header
hardening, cube/array slice decode `tp_ktx2_decode_slice_rgba8`, KV data
`tp_ktx2_kv_lookup`, and float decode `tp_ktx2_decode_*_rgbaf`) plus a
**complete decoder set**: new
`texcomp/src/texcomp_bc6h_decode.c` and `texcomp/src/texcomp_etc2_decode.c` mean
BC1/BC3/BC5/BC6H/BC7, ETC2/EAC, ASTC LDR+HDR and `uni` all decode, so
`tp_ktx2_decode_level_rgba8` now handles every LDR codec (previously only
uni/BC7/ASTC). Also picks up the BC5_SNORM, ETC2 differential-mode and ASTC-HDR
CEM15 fixes. Earlier imports: `14a2c9d`, `b113e0e`, `1d6ad64` (branch
`texcomp-ktx2-reader`, now merged).

Keep it pristine: prefer fixing bugs upstream and re-syncing over patching
here. If a local patch becomes unavoidable, list it under "lightusd-local
patches" below.

## What the libraries are

- `resize/` — **tir**: content-aware separable image resizer (U8/U16/F16/F32,
  1–4 interleaved channels; box…lanczos3/kaiser filters; clamp/reflect/wrap
  edges; premultiplied-alpha filtering; normal-map/heightmap content modes;
  runtime x86 SIMD dispatch, NEON, opt-in SVE and threads). Header:
  `resize/include/tir.h`.
- `texcomp/` — GPU block-texture compression: BC1/3/5/6H/7, ETC2/EAC,
  ASTC LDR+HDR, `uni` transcodable intermediate, DDS/KTX memory writers.
  Header: `texcomp/include/texcomp.h`.
- `texpipe/` — mip/orchestration layer on tir + texcomp: sRGB-aware and
  alpha-coverage-preserving mip chains, normal-map mips + Toksvig roughness,
  dilate/gutter, cube/octa seam fixup, per-level compression, KTX2 array
  writer. Header: `texpipe/include/texpipe.h` (includes `tir.h`+`texcomp.h`).
  **KTX2 reader** (`tp_ktx2_read`, `tp_ktx2_decode_level_rgba8`,
  `tp_ktx2_write_uni`): the consumer side of the writers — parse a KTX2, map its
  VkFormat to a codec (or detect the private ASTC-backed `uni` intermediate at
  `vkFormat==0` with its UNSPECIFIED-model DFD), then decode-to-RGBA8 (uni / BC7 / ASTC LDR) or hand the blocks
  to `texcomp`'s `tc_uni_transcode_*` for per-device transcoding.
  Supercompression scheme 0 is zero-copy; scheme 2 read/write is available
  through caller-supplied Zstd callbacks, keeping textools itself independent of
  a particular Zstd implementation. BasisLZ remains unsupported by native
  textools and is handled by Three.js KTX2Loader in the web loader.
- `envmap/` — IBL: equirect/cube/octahedral projections + conversion,
  GGX prefiltered specular cube mip chain, diffuse irradiance cube,
  split-sum BRDF LUT, spherical harmonics/gaussians. Header:
  `envmap/include/envmap.h` (includes `tir.h` for the allocator only).

Dependency graph: tir and texcomp are leaves; texpipe needs both; the envmap
library needs tir only. No dependency on tinyexr itself, zstd, or astcenc in
the imported library sources; no `<stdio.h>` in library code (upstream
enforces this).

## Imported / skipped files

Imported per tool: `include/*.h`, `src/*.c` (incl.
`texcomp/src/texcomp_astc_decode.c`, the ASTC LDR decoder — added in the
2026-07-11 re-sync) + internal headers/`.inc`,
license files, and the self-contained plain-`main()` test programs
(`resize/tests/{tir_test,tir_f16_test}.c`, `texcomp/test/test_texcomp.c` +
`*_ref_decode.h`, `texpipe/test/test_texpipe.c`,
`envmap/test/{test_envmap,test_pbr}.c`).

Skipped: all CLIs (`tir_resize_main.c`, `texcomp_cli.c`, `texpipe_cli.c`,
`envmap_cli.c` — they include tinyexr's `exr.h` and zstd), `bench/`, fuzzers,
test corpora, the optional astcenc C++ ASTC backend, and upstream's optional
Node/Puppeteer/Zstd Three.js interop harness. The equivalent lightusd browser
gate is registered by `web/CMakeLists.txt`.

## Build (lightusd-local; upstream has no CMake)

Gated by `LIGHTUSD_WITH_TEXTOOLS` (top-level `CMakeLists.txt`), which builds
the `lightusd_textools` static library from all four `src/` dirs with the
four `include/` dirs public. Notes:

- Only `resize/src/tir_kernels_sve.c` needs `-march=armv8-a+sve`
  (aarch64 + compiler-flag check; runtime HWCAP-gated, stub otherwise).
  All other x86/NEON dispatch is runtime/`__attribute__((target))`-based and
  needs no arch flags; on MSVC the target attributes compile out and
  intrinsics build unconditionally.
- Threads: `TIR_ENABLE_THREADS` + `TIR_THREADS_PTHREAD` are defined on UNIX;
  serial elsewhere.
- Consumers: `lusdview` (define `LUSDVIEW_WITH_TEXTOOLS`) via
  `examples/lusdview/texture_tools.{hh,cc}`; ctest targets `textools-*`.

## lightusd-local patches (must survive a re-sync)

- `texcomp/src/texcomp_astc.c`, `tc_astc_bitmap_mismatch_popcnt`: call
  `tc_popcount64()` instead of a bare `__builtin_popcountll`. The function is
  compiled whenever `TC_X86` is defined and `TC_TARGET` compiles away on MSVC,
  so upstream does not build with MSVC (lightusd `6a2e6f9f2`; still needed at
  upstream `96dabe5`). Under GCC/Clang `tc_popcount64` is the same builtin, so
  the POPCNT-targeted body is unchanged. Upstream fix pending.

## Re-sync procedure

1. In the upstream worktree, note the new commit hash.
2. Re-copy the imported file list above (`rsync`/`cp` from
   `<upstream>/tools/{resize,texcomp,texpipe,envmap}`), excluding the skipped
   files.
3. Review `git diff`; re-apply any local patches listed above.
4. Rebuild and test:
   `cmake --build build_ninja --target lightusd_textools && ctest --test-dir build_ninja -R 'textools-' --output-on-failure`
   then `ctest --test-dir build_ninja -R 'lusdview-' --output-on-failure`.
5. Update the import commit + date at the top of this file.

## Licenses

- `resize/` (tir): **BSD-3-Clause** (SPDX tags in sources; `resize/LICENSE`
  is a copy of the tinyexr root LICENSE — upstream keeps no per-subdir
  license file for `tools/resize`).
- `texcomp/`, `texpipe/`, `envmap/`: **Apache-2.0** (per-subdir `LICENSE`;
  `texcomp/NOTICE.md` carries third-party attributions — astcenc port
  credits, bcdec/Basis Universal/QuickBC7 concept credits — and must be
  preserved).

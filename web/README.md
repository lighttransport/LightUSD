# LightUSD JS/WASM

## Demo app

See `demo` folder.
Its build uses the local next WASM module and also builds the deprecated
combined module for the backend comparison page and LightRT worker. Emscripten
is required. The online viewer composes references and payloads during loading
through the next flattening API.

## Technical note on JS/WASM version of LightUSD

Please see wiki https://github.com/lighttransport/LightUSD/wiki/WASM-and-JavaScript-module

## For library developers

See `js` folder for JS codes.

## Building WASM module

Emscripten and `emcmake` are required. LightUSD uses C++20 coroutines for
async work across the JS/WASM boundary without requiring Asyncify or JSPI.

### Module variants: legacy / next

LightUSD has two USD cores. The published loader is next-first and the build
produces separate legacy and next products:

| Variant | CMake option | Module (`js/src/lightusd/`) | Contents | .wasm (MinSizeRel) |
|---|---|---|---|---|
| **legacy (deprecated, compile-only)** | `-DLIGHTUSD_WASM_PRODUCT=legacy` | `lightusd_legacy.js/.wasm` | Compatibility loader and legacy Tydra RenderScene conversion; not published | varies by toolchain |
| **core** | `-DLIGHTUSD_WASM_PRODUCT=core` | `lightusd.js/.wasm` | LightUSD core + Tydra (USDA/USDC/USDZ parse, composition, render extraction, streaming) | varies by toolchain |
| **combined** (compatibility-only) | `-DLIGHTUSD_WASM_PRODUCT=combined` | `lightusd_combined.js/.wasm` | Both APIs in one module for transition testing; not the published default | varies by toolchain |

Each variant emits a distinctly-named module, so builds never clobber each
other's output. `LIGHTUSD_WASM64=ON` combines with any variant and appends
`_64` to the module name (`lightusd_64`, `lightusd_wasm_64`, or
`lightusd_combined_64`).

- **core** (`src/`) is the default AOUSD-aligned implementation (parser, crate,
  PCP composition, evaluation, validation, and writers), exposed through
  Tydra and `RenderStream`.
- **legacy** (`src/*.cc`) remains available in the package for compatibility
  and continues to receive CI coverage (build-only). New integrations should use core.

### Standard WASM32 build (2GB memory limit)

Build the default core product:

```bash
$ emcmake cmake -S web -B web/build_ninja -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel
$ cmake --build web/build_ninja
```

### Core product

```bash
# core -> js/src/lightusd/lightusd.js/.wasm
$ emcmake cmake -S web -B web/cmake-build-core -G Ninja \
    -DLIGHTUSD_WASM_PRODUCT=core -DCMAKE_BUILD_TYPE=MinSizeRel
$ cmake --build web/cmake-build-next
```

### WASM64/MEMORY64 build (8GB memory limit)

```bash
$ ./bootstrap-linux-wasm64.sh
$ ninja -C build_64       # or: cmake --build build_64
```

### Build type & generator (.wasm size)

The package build script uses the **Ninja** generator with
**`CMAKE_BUILD_TYPE=MinSizeRel`**. MinSizeRel
applies emscripten's link-time `-Oz` (and drops runtime assertions), which is
what keeps the module small. See the measured legacy and core sizes below. A plain
`Release` build does **not** apply link-time size optimization (emscripten links
at `-O0`, pulling debug system libs + assertions), and produces a substantially
larger `.wasm` from the same code. Always ship MinSizeRel.

For an experimental speed-over-size build (larger `.wasm`, no `-Oz`/assertions),
use `bootstrap-linux-release.sh` / `bootstrap-linux-wasm64-release.sh`. The
`*-ninja.sh` scripts build the same MinSizeRel config into separate
`build_ninja` / `build_64_ninja` directories.

### Memory Limit Defaults

- **WASM32 (standard)**: 2GB default memory limit
- **WASM64 (MEMORY64)**: 8GB default memory limit

The JavaScript wrapper automatically uses the appropriate native default based on the build architecture.

**Note**: WASM64/MEMORY64 requires browsers with MEMORY64 support (Chrome 109+, Firefox 102+ with flags enabled).

The generated modules are written to `web/js/src/lightusd/`. Use
`web/npm/build-wasm.sh` for the reproducible core + MCP wasm32/wasm64 build.

## Known Issues

## Note

* asyncify is disabled since it increases code size ~2.5x

### Code size

The following sizes were measured on 2026/09/07 with Emscripten 4.0.14,
WASM32, Ninja, and `CMAKE_BUILD_TYPE=MinSizeRel`:

| Variant | `.wasm` | `zstd -19` |
|---|---:|---:|
| legacy (`lightusd.wasm`) | 6,551,876 bytes (6.25 MiB) | 1,321,824 bytes (1,290.84 KiB) |
| next (`lightusd.wasm`) | 1,671,646 bytes (1.59 MiB) | 557,584 bytes (544.52 KiB) |

### zstd compression

We recommend using zstd compression for the WASM binary in deployment. The
measured results above use compression level `-19`.

### Prepare wasm.zstd

```
$ zstd -19 lightusd.wasm
```

See js/src/lightusd/LightUSDLoader.js to how to load zstd compressed wasm.

### stack size

128k => ok.
64k => ok.

## npm packaging

Use `web/npm` for packaging metadata and staging scripts.

```bash
cd web/npm
npm run build
npm run validate
```

The GitHub publish workflow is in `<lightusd>/.github/workflows/wasmPublish.yml`.

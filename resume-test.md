# Regression testing checkpoint

Updated 2026-10-19. The legacy sanitizer gate that was interrupted earlier is
now **complete**. HIP remains excluded. Nothing is committed; the large
C11/storage/TU/WASM/Python refactor and these fixes are uncommitted on `dev`
(base `abc530f7c`).

## Result

Aggregated by unique CTest name (original sweep → interrupted retry → resume
runs; latest wins): **366 non-HIP CTests: 361 passed, 5 skipped, 0 failed**,
with **zero project ASan/UBSan/LSan reports** in every resume run. The
1,084-case `unit-test-lightusd` passes under all three sanitizers.

Justified skips:

| CTest | Reason |
| --- | --- |
| `lusdview-island-production-smoke` | External `ISLAND_USD` asset unavailable. |
| `lusdview-openchess-path-trace` | Opt-in (`LUSDVIEW_RUN_OPENCHESS_PATH=1`). |
| `lusdview-usd-assets-golden` | Opt-in (`LUSDVIEW_RUN_GOLDEN=1`). |
| `lusdview-usd-assets-broad-smoke` | Opt-in (`LUSDVIEW_RUN_USD_ASSETS_BROAD=1`). |
| `lusdview-usd-assets-animation-smoke` | Opt-in (`LUSDVIEW_RUN_USD_ASSETS_ANIMATION=1`). |

Earlier normal-build broad/animation coverage remains the record for the
opt-in profiles.

## Fixes and findings in this resume

1. **tiny_dng misaligned load (real bug, fixed).** `IsDNGFromMemory` and
   `LoadDNGFromMemoryImpl` now `memcpy` the byte-symmetric TIFF magic.
   New `tinydng_odd_offset_probe_test` reproduces the UBSan report on the old
   code and passes on the fix. `lusdview-displacement-udim` and
   `lusdview-texture-semantic-rt-loader-parity-vk-rt` pass. Normal build and
   unit suite pass; combined wasm32/memory64 modules rebuilt and
   `next-schema-utilities-behavior-parity` passes on both. The Python extension
   does not link tiny_dng.
2. **CUDA `cuInit` OOM (instrumentation config).** ASan's protected shadow gap
   blocks the CUDA UVM reservation (`cuInit` = 2). A standalone probe confirms:
   plain = 0, ASan = 2, ASan + `protect_shadow_gap=0` = 0. With that option all
   eight CUDA viewer tests pass for real, and the 456-byte `libcuda` leak
   report does not occur.
3. **Instrumentation budgets.** Sanitizer-only CMake settings (normal builds
   unchanged): AOUSD `TIMEOUT 2700` (83/83 fixtures pass both loaders — 51 in
   CTest before the old 1,200 s limit, 32 via the same checks directly);
   `lusdview-usdz-deferred-payload` uses new
   `LUSDVIEW_DEFERRED_PAYLOAD_TIMEOUT_MS=120000` (harness default 10 s; normal
   build passes in 5 s); `lusdview-rt-geomsubset-material`
   `LUSDVIEW_RENDER_TIMEOUT=240s`; `lusdview-openchess-materials`
   `LUSDVIEW_OPENCHESS_TIMEOUT=900s`. Assertions unchanged.
4. `lusdview-camera-stereo-contract` and `mcp-test` pass under the complete
   leak profile; no stereo code change was needed.

## Evidence (ignored build directories)

Under `build_ninja/regression-flatten-sanitize/sanitizer/`:

| Evidence | Path |
| --- | --- |
| Aggregate (all 366 tests, with source run) | `resume/aggregate.json` |
| Resume runs (ctest.log/xml, asan/, ubsan/ each) | `resume/{pkg-image,cuda,retry3,deferred2,aousd,aousd-rest,viewer31,openchess,unit-full}` |
| Pre-fix tiny_dng reproduction (expected report) | `resume/unit-odd-prefix/ubsan/` |
| CUDA probe | `resume/cuda/probe.c` |
| Runner (command.json env + complete LSan profile + `protect_shadow_gap=0` + fresh reports) | `run2.py`, `ct.sh NAME REGEX` |
| Original sweep / interrupted retry | `{summary.json,ctest.xml,ctest.log}`, `fixed-suite/` |

## Runtime setup notes

- Use `run2.py`/`ct.sh`, not the old `retry.py` (which forces the incomplete
  `lsan-gpu-external.supp`). Known driver-only leaks (D-Bus/Vulkan,
  `libnvidia-glcore`, `libGLX_nvidia`) are covered by
  `lsan-graphics-complete.supp`; project leak checks stay enabled.
- ASan + CUDA requires `protect_shadow_gap=0`.
- Unset stale `DISPLAY`, keep NVIDIA PRIME/ICD env, run GPU tests serially
  under `xvfb-run -a -s '-screen 0 1280x800x24'`, exclude HIP with
  `-E '(^|[-_])hip([-_]|$)'`. The aggregate covers the original 366-test
  non-HIP selection; the regenerated tree now registers 370 tests in total.
- Avoid concurrent heavy builds during the AOUSD sweep; CPU contention made it
  overrun 1,200 s.

## Opt-in profiles under sanitizers (2026-10-19, stopped by request)

Run with all four `LUSDVIEW_RUN_*` switches via `resume/optin.sh`; stopped at
the user's request during the golden profile. Golden baselines were verified
unchanged (`resume/goldens.sha256`).

| CTest | Result |
| --- | --- |
| `lusdview-openchess-path-trace` | **Failed (open finding).** Vulkan PT matches the reference (SSIM 0.9997, nRMSE 0.006); the CUDA PT candidate missed thresholds (SSIM 0.645, nRMSE 0.252, relative mean error 0.887) after 368 s, not a timeout. 0 sanitizer reports. Not yet known whether this reproduces on the normal build. |
| `lusdview-usd-assets-golden` | Interrupted after ~55 min; 0 sanitizer reports so far. |
| `lusdview-usd-assets-broad-smoke` | Not run. |
| `lusdview-usd-assets-animation-smoke` | Not run. |

A CPU-only background process kept load at ~31 during these runs, so timings
from this session are pessimistic.

## Remaining

Nothing required for the default sanitizer gate. Next:

1. Rerun `lusdview-openchess-path-trace` with `LUSDVIEW_RUN_OPENCHESS_PATH=1`
   on the normal build (`build_ninja`) to classify the CUDA PT mismatch as a
   CUDA path-tracer bug or a sanitizer-only effect; inspect the CUDA image.
2. Finish the golden, broad and animation opt-in profiles on an idle machine.
3. Provide `ISLAND_USD` for the production smoke if wanted.

Commit/push only on separate request.

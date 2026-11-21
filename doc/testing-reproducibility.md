# Reproducible Verification

The repository-wide verification harness is driven by
[`scripts/verify.sh`](../scripts/verify.sh). It separates preparation from
testing: preparation checks out the pinned external repositories and builds
ignored artifacts, while test actions consume that prepared cache with no
network updates.

## Clean full run

Use a new cache for an independent reproduction. The cache may be placed
elsewhere with `LIGHTUSD_VERIFY_CACHE`.

```bash
export LIGHTUSD_VERIFY_CACHE=/tmp/lightusd-verification-clean
scripts/verify.sh prepare --profile full
scripts/verify.sh test --profile full --offline --software
```

The preparation manifest is
[`tests/verification/manifest.json`](../tests/verification/manifest.json).
It pins MuJoCo, MuJoCo Menagerie, USD-WG assets, and the OpenUSD v26.05 oracle.
The web dependencies are locked by `web/js/package-lock.json` and installed
with `npm ci`.

The full profile covers:

- native CMake/CTest tests and the USD-WG corpus parser;
- the Debug `src/` CMake/CTest suite;
- web Node/WASM tests, MuJoCo physics, Menagerie MJCF/USD/MJCF closure,
  five-model URDF browser rendering, and the full OffscreenCanvas worker sweep;
- USD-WG asset parsing; and
- LightUSD/OpenUSD `usdcat` comparison and checker parity.

Browser phases need loopback access for Vite and Puppeteer. In restricted
sandboxes, run the test command with the environment's approved loopback
permission. Software mode avoids requiring a physical GPU.

## Focused preparation and tests

The same entrypoint supports smaller slices:

```bash
# MuJoCo checkout/build and direct physics smoke test.
scripts/verify.sh prepare --target mujoco-wasm
scripts/verify.sh test --target mujoco-wasm --offline

# Physics smoke plus all pinned Menagerie closure cases.
scripts/verify.sh test --target web-physics --offline --software

# Individual dependency preparation.
scripts/verify.sh prepare --target menagerie
scripts/verify.sh prepare --target assets
scripts/verify.sh prepare --target openusd
scripts/verify.sh prepare --target wasm --offline
```

Preparation scripts also expose checkout/build boundaries:

```bash
scripts/prepare-mujoco-wasm.sh --checkout-only
# First configure/build may fetch MuJoCo's pinned CMake dependencies.
scripts/prepare-mujoco-wasm.sh --build-only
# Rebuild without Git or CMake FetchContent network access once cached.
scripts/prepare-mujoco-wasm.sh --build-only --offline
scripts/prepare-usd-assets.sh --checkout-only
```

`scripts/prepare-mujoco-wasm.sh` checks out and builds the pinned manifest
ref into the verification cache. This pin upgrades the fork's MuJoCo base
from 3.7.0 to 3.10.1 and includes the physics-only COM-Jacobian and spatial-
tendon bindings used by LightUSD. The pinned head also gates emscripten's
`--emit-tsd` behind `MUJOCO_WASM_EMIT_TSD` (default OFF) because the typings
codegen shells out to `tsc --outFile`, which TypeScript 5.x removed. Point
the physics tests at the built artifacts with
`MUJOCO_WASM_DIR=<cache>/mujoco/wasm/dist`.

`--offline` requires both the pinned MuJoCo checkout and its CMake
FetchContent dependencies to have been populated by an earlier online build.
It disables Git updates and configures CMake with
`FETCHCONTENT_FULLY_DISCONNECTED=ON`.

Use `scripts/verify.sh doctor --profile full` to check required host tools and
print the manifest digest and cache location. Each action writes a JSON report
under `<cache>/reports/`.

## Recorded clean-run result

On 2026-08-07, the full offline profile passed from a fresh cache:

- native CTest: 37/37 passed;
- next CTest: 34 passed, 1 intentional skip;
- Menagerie closure: 67/67 passed;
- URDF browser sweep: 5/5 passed;
- OffscreenCanvas worker sweep: 67/67 passed;
- USD-WG asset parser: 0 hard failures; and
- OpenUSD comparison: 446 compared, 0 failed, 2 documented skips.

Generated builds, downloaded repositories, browser output, reports, and corpus
results remain outside Git or are covered by repository ignore rules.

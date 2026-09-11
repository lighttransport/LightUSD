# xatlas vendored dependency

This directory contains the xatlas source used by Lucia Code.

- Upstream: https://github.com/jpcy/xatlas
- Pinned commit: `f700c7790aaa030e794b52ba7791a05c085faf0c`
- Snapshot date: 2022-07-26
- License: MIT; see `LICENSE`

Only the core library sources are vendored. Viewer, example, test, and image
loader sources are intentionally excluded. xatlas supplies chart generation,
parameterization, packing, progress callbacks, and seam vertex remapping. The
LightUSD adapter remains responsible for checked mesh conversion, aligned
primvar transfer, deterministic option normalization, and USD authoring.

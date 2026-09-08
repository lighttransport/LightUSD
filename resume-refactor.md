# LightUSD refactor checkpoint

## Accepted stopping point

The user accepted approximately **1.6 MB raw WASM** and requested that TU and
WASM-size optimization stop after finishing the PCP TU refactor. That refactor
is finished. The earlier <512 KiB goal is superseded. Do not resume optimization,
regroup binding TUs, change optimization flags, or pursue additional buffer/API
migrations without a new request.

The working tree contains substantial pre-existing and new uncommitted work.
Preserve it; do not reset or clean the tree. No commit or push was performed.

## Finished PCP refactor

Private implementation types live in `src/next/pcp/cache-internal.hh`.
Non-template compiled boundaries separate:

- `cache.cc`: public forwarding API and remaining coordination.
- `cache-arcs.cc`: arc and variant expansion.
- `cache-relocates.cc`: relocation and ancestral source resolution.
- `cache-layers.cc`: layer-stack construction and expression fingerprints.
- `cache-opinions.cc`: strong-to-weak opinion composition.
- `cache-storage.cc`: container construction/destruction, including worker caches.

The existing composition algorithms, ownership and threading behavior are
preserved. The private implementation header must not become a consumer header.
No source-local optimization workaround was added.

## Final measurements

Emscripten 4.0.14, MinSizeRel, SIMD, wasm32, without LTO:

| Item | Result |
| --- | ---: |
| Raw next + Tydra-next WASM | 1,626,756 bytes |
| Comparable pre-refactor WASM | 1,689,884 bytes |
| Raw size reduction | 63,128 bytes (3.7%) |
| Memory64 WASM | 1,827,671 bytes |
| Original PCP cache TU, isolated pass | 9.00 s |
| Final cache.cc, isolated pass | 6.25 s |
| Final cache-arcs.cc | 5.43 s |
| Final cache-relocates.cc | 4.55 s |
| Final cache-layers.cc | 4.38 s |
| Final cache-opinions.cc | 4.21 s |
| Final cache-storage.cc | 4.13 s |

Final PCP timing is one cache-disabled serial pass, not a multi-run median.
Earlier three-run measurements and the broader production sweep are described
in `doc/refactor-c-core.md`. Splitting reduces the slowest TU but increases
aggregate compilation work and aggregate pre-link object text; do not claim a
whole-product clean-build speedup. The pre-refactor product retained its old
Tydra coordinator's source-local optimization override; the current build does
not. A separate LTO trial at the preceding checkpoint produced 1,667,514 bytes,
so LTO was not adopted.

## Verification

After the final PCP lifecycle extraction:

- Native next CTest: 37 passed; one optional conformance case skipped.
- Threaded ASan/UBSan CTest: 37 passed; the same optional case skipped.
- wasm32 and memory64 products built successfully.
- wasm32 USDA composition suite passed.
- Memory64 next-only composition/variant/diff/validation cases passed;
  dispatch lifetime/JSON and Tydra/flatten smoke suites passed.
- The mixed memory64 composition suite's legacy case fails because the
  existing legacy module lacks `nextFlattenAsyncBegin`; it is outside the
  next-only product and was not changed.

Before the final lifecycle-only extraction: the rebuilt Python extension passed
133 tests with seven skips; legacy MiniJSON unit tests passed; the full Node
profile using the rebuilt next module passed 18 suites, with the helper suite's
three previously observed EXR failures remaining. The standalone checker built,
dumped the immutable rule registry and validated the cube fixture.

These results do not declare the full browser/legacy parity gate green.

## Other changes retained from this work

See `doc/refactor-c-core.md` for the API changes: C-dispatched next WASM classes
(still using emval internally), split render bindings, compiled POD chunk
storage with explicit mutation, Dragonbox-backed MiniJSON number serialization,
and split validation with an immutable pointer/count rule registry.

The larger raw-value-buffer, portable POD binding ABI and default-product
migrations remain unimplemented roadmap items, not follow-up instructions.
A mixed-function GCC buffer microbenchmark showed a register-spill slowdown,
while an isolated read kernel did not and a generated 30,000-triangle WASM
conversion measured about 47 ms versus 49 ms with equal heap usage. Comprehensive
runtime parity is not established; this caveat is retained rather than treating
the size acceptance as proof of runtime parity.

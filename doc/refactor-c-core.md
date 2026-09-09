# C-style core consolidation

## Accepted stopping point

The user accepted approximately 1.6 MB raw WASM and requested that TU/size
optimization stop after finishing the PCP TU refactor. The earlier 512 KiB
goal is no longer an acceptance criterion. Remaining architecture items below
are roadmap notes, not instructions to continue optimization or switch defaults.

The target is one default implementation based on `src/next`, compiled as
C++17 with explicit storage and non-template algorithms. Legacy remains an
explicit compatibility product. The native and WASM defaults must not switch
until the current default products' supported capabilities have regression
coverage on next. API changes are allowed; removing capabilities is not an
optimization.

## Implementation status

| Milestone | Status / remaining work |
|---|---|
| Reproducible measurements | `scripts/bench-compile.py` measures clean Ninja targets and isolated translation units, fingerprints the working tree, records compiler commands/versions, timings, object sections, and raw/gzip artifact sizes. Whole-product before/after gates remain necessary. |
| Immutable built-in type metadata | Implemented: one descriptor table supplies names, layouts, components, and classifications. Constant-initialized name indexing replaces the separate allocated parser map. |
| Shared array ownership | Implemented: one tagged backing and compiled atomic retain/release/detach path replaces the virtual array subclasses and per-type shared ownership machinery. Vector storage/accessors remain transitional to preserve zero-copy adoption. |
| C-style worker dispatch | Implemented: `TaskArena` stores a context pointer plus function pointer instead of `std::function`; the synchronous template adapter preserves lambda call sites without retaining type-erased callback machinery in the worker arena. |
| Shared string index | Implemented: an overflow-checked open-addressed integer index references caller-owned keys. Dictionaries and property/schema-name tables use it without duplicate key strings; dictionary operations compile once outside the public header, with read-only entry iteration and indexed mutation. Immutable threaded snapshots retain stable name views. |
| Shared pipeline source definitions | Standalone next and combined WASM consume the same pipeline source list. Each parent continues supplying its own shared support code, including LZ4. |
| Explicit buffers and scene records | Pending: replace transitional vectors with checked aligned buffers, pooled string arrays and owner/index/generation scene handles. Preserve copy-on-write, lazy Crate ownership, cold metadata, and ordered dictionaries. |
| Shared binding boundary | Partial: next WASM class registration is replaced by four C exports and JS wrappers, with generation-checked object IDs. Existing method coverage is retained. The internal argument/result bridge still uses emval; a portable POD ABI shared with native/Python remains pending. |
| Tydra chunk ownership | Implemented: one compiled, element-size/alignment-aware storage engine replaces per-type vector/shared-pointer chunk ownership. The thin typed facade provides read-only indexing and explicit mutation. |
| Product selection and defaults | Root CMake accepts `LIGHTUSD_NATIVE_PRODUCT=next` and returns before configuring legacy dependencies; `legacy` remains the default. Full parity and the default switch remain pending. No implicit legacy fallback is permitted in the final next product. |

## Coverage gates for consumer migration

| Capability | Existing next coverage / migration requirement |
|---|---|
| Values and metadata | `test_next`, `test_value_dict`, `test_crate_dict`, `test_roundtrip_fidelity`; retain roles, blocked opinions, nested/unknown metadata, and byte layouts. |
| USDA / USDC / USDZ I/O | Reader, writer, roundtrip, malformed Crate and crash-replay suites; port any default legacy corpus behavior missing in next before switching defaults. |
| Composition / authoring | PCP, parallel PCP, load rules, composition, layer diff, array edits and conformance suites; compare bindings' arcs, variants, instances, expression variables and time evaluation. |
| Rendering / schemas | Next schema and Tydra suites plus viewer/renderer regressions; verify non-mesh geometry, materials, textures, skinning, animation and physics/format bridges. |
| C / Python | `test_lightusd_c` and `python/tests`; preserve status reporting, retained owners, borrowed views, mutation invalidation and multi-interpreter/thread behavior. |
| WASM / JS | Full `web/js` regression gate, including conversion, dependency-layer sessions, render streaming, subdivision, Worker rendering and physics. Test both wasm32 and memory64. |

These are subsystem gates, not a declaration of complete legacy/next parity.
The remaining binding migration requires a method-level inventory of every
export in both current modules and its C API replacement before removal.

## API migration in this change

`TypeInfo` is immutable metadata. Its `construct`, `destruct`, `copy`, `move`,
and `equals` callbacks and `RegisterTypeInfo` have been removed. The old registry
had no extension ID space: every valid nonzero ID was already occupied. Use
`Value` for ownership/copy/equality and `GetTypeSize`/`GetTypeAlignment` for
layout queries. `InitTypeRegistry()` remains a compatibility no-op.

`GetTypeIdFromName(data, length)` accepts a bounded, non-NUL-terminated name.
Existing NUL-terminated callers remain supported. Type IDs and USD names have
not been renumbered.

`Dict::entries` is replaced by the read-only `entries()` accessor. Iterate
through that accessor and mutate values through `find` or `set`; direct key
replacement/reordering is no longer supported. `Dict::index_` is private, and
name-interning tables are non-copyable. Index allocation failure falls back to
searching authoritative storage; it cannot drop a value or interned name. This
does not yet make remaining vector/deque allocations recoverable on
out-of-memory.

`PropNameTable::get` now returns `std::string_view`, replacing the former
`const std::string &`. The view is stable for the lifetime of the table,
including after later names are interned; callers that need owned storage must
make an explicit `std::string` copy. `PrimSpec` keeps its cold declared
property-type spellings as owned strings so `property_type_name()` can retain
its pointer-returning API.

### Tydra buffers and next WASM dispatch

`ChunkedArray<T>` requires trivial elements. Allocation, reference counting,
detachment, compaction and bulk copying compile once in `chunk-storage.cc`.
Over-aligned elements and exact-sized tails are supported. Allocation failure
from reserve/resize/append remains recoverable; explicit reference-returning
mutation aborts if detachment cannot allocate, so reserve first when failure
must be reported to the caller.

`operator[]`, `at`, `front`, `back`, `chunk_data`, and ordinary iteration are
now read-only, even on mutable arrays. Use `mutable_at`, `mutable_chunk_data`,
or `mutable_begin`/`mutable_end` to write. Reads never detach shared geometry.
Borrowed pointers/iterators must not survive mutation, sharing, or destruction
of their owner. No source-local optimization override is used for the Tydra
coordinator.

The next WASM post-JS wrapper keeps the existing converter, flatten session,
subdivision, render-stream methods and aliases. Objects must still be explicitly
`delete()`d; `isDeleted()` is supported. Embind-specific conveniences such as
`clone()` and `deleteLater()` are not part of this API. Native IDs are uint32
on both wasm32 and memory64, and exhausted generations retire instead of
wrapping. The four exports in `web/binding-next-api.h` are an internal emval
bridge, not the stable native C API; raw emval IDs are trusted runtime values.
Do not call destruction directly from an active object's callback; the JS
wrapper rejects this operation.

Binding scene JSON now uses MiniJSON, including explicit non-finite-to-null
conversion. MiniJSON's finite-double serializer uses the existing vendored
Dragonbox implementation and integer formatting, avoiding libc++'s separate
large floating-format tables. Numeric values are preserved, but equivalent
JSON number spelling and object ordering are not promised byte-for-byte.
`RenderStream` implementation is split into small private translation units;
no per-method Embind registration templates remain.

PCP arc expansion, relocation, layer-stack construction and opinion composition
now have out-of-line private implementations. Shared implementation types live
in `pcp/cache-internal.hh`; do not expose this header to consumers. Validation
has separate physics/lux and report/registry translation units with a private,
non-template helper boundary.

PCP container construction and destruction are also compiled once in
`pcp/cache-storage.cc`, including for temporary parallel-worker caches.

`GetValidationRuleTable(size_t* count)` returns a borrowed pointer to immutable
`ValidationRuleInfo` records instead of a vector reference. Records have process
lifetime, and `count` may be null. The checker is migrated to indexed traversal.
Both the rule registry and the small severity-upgrade list use constant data,
with no dynamic initialization or hash-table allocation.

## Measurements

The root build can select the standalone next implementation explicitly:

```sh
cmake -S . -B build_ninja/next -G Ninja \
  -DLIGHTUSD_NATIVE_PRODUCT=next -DLIGHTUSD_BUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build_ninja/next
ctest --test-dir build_ninja/next --output-on-failure
```

This exposes `lightusd_next` / `lightusd::next`, the next C API and render
targets, and next tools. It does not alias legacy C++ interfaces. The root
test/thread options initialize their `LIGHTUSD_NEXT_*` counterparts; explicit
next options take precedence. Other legacy feature options do not configure
the selected next product. Use a separate build directory for each product.

Configure a separate Ninja tree with `CMAKE_EXPORT_COMPILE_COMMANDS=ON`. Do not
run timing passes concurrently with other builds or benchmarks. Reports contain
local command paths and belong in ignored build directories, not public history.

```sh
cmake -S src/next -B build_ninja/refactor-next -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DLIGHTUSD_NEXT_BUILD_TESTS=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
python3 scripts/bench-compile.py --build-dir build_ninja/refactor-next \
  --target lightusd_next --repeat 3 --jobs 16 \
  --artifact build_ninja/refactor-next/liblightusd_next.a \
  --output build_ninja/refactor-next/clean-build.json
python3 scripts/bench-compile.py --build-dir build_ninja/refactor-next \
  --source src/next/types/value.cc --source src/next/types/array-storage.cc \
  --source src/next/types/value-dict.cc --source src/next/core/string-index.cc \
  --source src/next/types/type-info.cc --source src/next/parser/value-parser-types.cc \
  --output build_ninja/refactor-next/core-compile.json
```

For WASM, configure through `emcmake` with MinSizeRel and
`LIGHTUSD_WASM_PRODUCT=next`. Set `LIGHTUSD_WASM_OUTPUT_DIRECTORY` to an absolute
path inside the measurement build tree to avoid replacing the application's
generated modules. Record the `.wasm` and `.js` files with `--artifact`.

Compare the sum of all affected translation units, including extracted files.
Object sections and final artifacts matter independently of object-file bytes.
RSS from the runner is the largest child process, not aggregate parallel-build
memory. Keep compiler/SDK versions, flags, feature coverage and parallelism
fixed. Evaluate LTO separately. Each optimization must improve its declared
metric beyond repeated-run noise; reproducible runtime or peak-memory regressions
over 5% block the milestone. Final acceptance needs smaller whole-product
build times, object text/data, and shipped WASM at equivalent coverage.

### Initial measurements (foundation changes)

GCC Release, 16 build jobs, three clean builds per version: median whole-core
build time was 24.89 s before and 24.76 s after, which is within measurement
noise. The six affected/extracted translation units totaled 3.27 s versus
3.57 s for the original three, with object text/data dropping from 79,689 to
68,394 bytes. The native archive shrank about 1%. MinSizeRel wasm32 decreased
from 1,680,837 to 1,676,336 bytes (about 0.27%); this is a small foundation
improvement, not completion of the size goal.

Seven alternating synthetic runtime runs measured median name interning plus
lookup at 45.3 ms versus 76.2 ms, dictionary insertion plus lookup at 95.4 ms
versus 104.7 ms, and array detachment at 82.3 ms versus 83.1 ms. Combined peak
RSS fell about 9.5%; `sizeof(Dict)` fell from 80 to 48 bytes on this platform.
These measurements precede the read-only dictionary accessor and root product
selector; rerun the commands above for the final configuration being evaluated.

## Validation of the foundation changes

### Final accepted checkpoint

The completed PCP extraction includes explicit out-of-line implementation
construction/destruction. Its final cache-disabled Emscripten 4.0.14 wasm32
pass measured cache/arcs/relocates/layers/opinions/storage at
6.25/5.43/4.55/4.38/4.21/4.13 seconds respectively, versus the original cache
TU's 9.00-second sweep result. Before the last lifecycle extraction, three-run
medians were 6.65/5.40/4.60/4.35/4.17 seconds for the first five files.
Validation main/report/physics medians were 5.90/2.01/2.28 seconds.
The 164-configuration initial production sweep found only the original PCP
and validation TUs above six seconds; those were subsequently decomposed.

Final normal wasm32 is 1,626,756 bytes (text 1,466,675; data 150,845), versus
the comparable pre-refactor 1,689,884-byte product. Memory64 is 1,827,671 bytes.
The separate LTO trial at the preceding checkpoint was larger, at 1,667,514
bytes, and was not adopted. Splitting improves maximum TU latency but increases
aggregate compile work and pre-link object text. No whole-product clean-build
speedup is claimed. These measurements do not authorize further optimization;
the accepted stopping point above supersedes the former size goal.

Final native and threaded ASan/UBSan suites each passed 37 runnable cases, with
one optional conformance skip. Both WASM widths built. wasm32 composition and
memory64 next-only composition/variant/diff/validation, dispatch and render smoke
checks passed. A mixed memory64 suite's legacy case lacks
`nextFlattenAsyncBegin` in the existing legacy module; no legacy rebuild or
workaround was introduced. Immediately before the lifecycle-only extraction,
Python passed 133 tests (seven skips), the legacy MiniJSON unit suite passed,
and the full Node profile passed 18 suites with the known EXR helper failures.

A mixed-function GCC buffer microbenchmark retained a register-spill slowdown;
isolated read kernels did not reproduce it. A generated 30,000-triangle WASM
conversion measured approximately 47 ms versus 49 ms at equal heap usage.
This is not comprehensive runtime parity. See `resume-refactor.md` for the
stopped-work handoff and remaining caveats.

### Earlier foundation validation

- Standalone next, root-selected next, and threaded ASan/UBSan builds each
  passed all 37 runnable CTest cases; one optional conformance case was skipped.
  A focused ThreadSanitizer run covered the changed storage and name-table code.
- The rebuilt Python extension passed 133 tests with seven skips.
- wasm32, memory64, and combined WASM products built successfully. Both pointer
  widths passed the next-only JS suite. The full Node profile passed 17 suites;
  the helper suite retained three EXR resize/transcode/roundtrip failures also
  observed before this core change. The full web gate additionally lacks the
  Menagerie dataset in this environment.
- The existing native build, including tools and viewer, completed. Its full
  CTest run had 251 passes, 21 skips, and six viewer failures; all six passed on
  targeted rerun after enabling configure-time NVIDIA detection outside the
  sandbox and installing Pillow. The llvmpipe fallback could not support the
  full OpenGL shader. No renderer source workaround was added.
- The newer local OpenUSD oracle found 450 equivalent USDA files and 223
  equivalent USDC files, with no unexpected output differences (six USDA
  expected failures, three expected differences, four USDC expected failures).
  One malformed USDA fixture was rejected by the oracle, so this is not an
  unconditional corpus pass.

The web failures and missing coverage mean the product parity gate is still
open. This change does not complete the raw-buffer, scene-handle, binding, or
default-product migrations listed above.

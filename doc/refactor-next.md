# refactor-next: src/next Optimization & Hardening Roadmap

> **Historical (pre-1.1).** This document describes the `src/next` / legacy
> split before LightUSD 1.1 promoted `next` to the core (`src/`) and moved the
> old implementation to `src/legacy/`. Paths and names below are as they were;
> see [core.md](core.md#migrating-from-next) for the mapping.

Status: **post-roadmap cleanup**. The original 10-phase roadmap has landed.
Use `scripts/run-next-checks.sh` for a focused standalone validation pass before
and after follow-up refactors.

Landed so far:
- **Phase 0** — this plan + `bench_pcp_compose` instrumentation + baselines.
- **Phase 1** — recursion/cycle hardening (ExpansionFrame stack-frame chain,
  ancestor-arc + namespace-depth guards, iterative SourcesForPath); lazy
  per-prim `values_`/`time_samples_`; dead `ValueStorage` byte API removed.
  Also fixed a parser bug (apiSchemas/variantSets dropped quoted names).
- **Phase 2** — three libFuzzer harnesses + `next_usdcat` + `next_corpus_parse`
  gate; 6 fuzzer-found crash/UB bugs fixed and pinned by a replay test.
- **Phase 3** — copy-on-write array storage in `Value` (12× memory cut on the
  clone benchmark).
- **Phase 4** — `FindSpecs` memoization (−24% BuildStage). GraftSubtree
  child-walk (M5) landed with a validity-guarded fallback. M3 landed 2026-07-16
  as open-addressed caches/memos instead of a full u32-interned rekeying:
  `StackSpecCache` (per-stack Specs memo, no per-call key copy),
  `Src::specs_`/`SpecsFor` (self-resetting per-Src memo), `SrcCache`
  (sources_cache), and `arc_target_memo_` (per-arc external-target resolution
  keyed by anchor+asset+expression-vars fingerprint). Island/ALab/Caldera
  load+compose −14–16%, byte-identical, TSan-clean — see
  doc/memory-and-performance.md §M3.
- **Phase 5 (core)** — stable variant-content instance key (fixes pointer
  aliasing S4); the instance key now folds in the accumulated variant selections
  and non-identity layer offsets (I1). Strongest-opinion tri-state
  `instanceable` landed (`instanceable_authored`, so authored `false` ≠
  unauthored — I2). Instance↔prototype **path-translation API landed** (I3):
  `Cache::TranslatePathToPrototype` (longest enclosing instance prefix, recursing
  through nested instances, empty when under none) and
  `TranslatePathFromPrototype` (single named instance); covered by
  `test_pcp.cc:test_path_translation` including a two-level nested case.
  PointInstancer typed accessors/transform computation (plus
  `ComputeMaskAtTime`) and lightweight Tydra draw-reference expansion now exist,
  including direct and inherited mesh material IDs, prototype-relative mesh
  transforms, and unresolved-prototype diagnostics on draw refs. Bounds-checked
  draw view resolution, O(1) per-instancer draw ranges, and draw/prototype
  binding validators are available; opt-in transformed mesh duplication is
  available for consumers that cannot use draw refs directly. (The M3 pcp
  hot-map work, once deferred, landed 2026-07-16 — see Phase 4 above.)
- **Phase 6** — payload LoadRules model (`pcp/load-rules.{hh,cc}`, a
  `UsdStageLoadRules` port); `LoadPayload(With/WithoutDescendants)`,
  `SetLoadRules`; `UnloadPayload` recomposes (S7 fix); BuildStage rebuilds
  prototype maps from scratch (fixes stale-grouping nondeterminism + payload-on-
  instance split).
- **Phase 7 (E1)** — USDA parser now honors arc list-op qualifiers
  (prepend->front, append->back, delete->remove, explicit->replace) within a
  spec.
- **Phase 7 (E4)** — typed composition diagnostics: `Cache::ErrorCode` +
  `Cache::CompositionIssue` accumulated on Impl; all err-emitting sites route
  through `AddIssue` (records a typed code + mirrors the message into the
  existing `err` out-param, no API break). Recorded even when `err==nullptr`;
  parallel-worker issues folded in at the post-join merge.
  `GetCompositionIssues`/`ClearCompositionIssues` exposed.
- **Phase 8 (arrays, M1/8.4)** — read+write lazy coverage for Vec4f/Quatf/
  Vec2-4d/Quatd/Matrix2-4d **and half-typed** arrays (Half/Vec2-4h/Quath, lazy
  + byte-exact passthrough; half↔float via crate-format.hh). TimeSamples sample
  values reuse the same UnpackValue→UnpackArray lazy path. Corpus now has
  **zero** "Unsupported array type" warnings.
- **Phase 8 (M4/8.1)** — PrimSpecMeta footprint split: cold fields
  (doc/comment/instance_prototype/apiSchemas/variantSets/variantSelections/
  relocates) moved behind a lazily-allocated `unique_ptr<PrimSpecMetaExt>`; hot
  fields (flags, 4 arc lists, legacy variantSelection, layer_offset) stay
  inline. `sizeof(PrimSpecMeta)` 344B→160B without ext (2.15×); ext (192B) paid
  only by the prim minority that carries cold data. `Layer::finalize()`
  shrink_to_fit's `prims_`.
- **Phase 8 (M6/8.2)** — pooled token storage (`TokenPool`: one blob + (off,len)
  spans) replaces the per-token `std::string` table in the crate reader: 8
  bytes/token + no per-token alloc, cutting the transient parse peak. Consumers
  copy by value (unchanged behavior); `CrateReader::tokens()` now materializes
  on demand (diagnostics-only). The `CrateDataSource` token table is never
  populated (token arrays decode eagerly), so it carries no steady-state cost.
- **Phase 8 (M7/8.3)** — mmap-backed `CrateDataSource`: a second read-only
  memory-map backing behind `base()`/`size()` (readers/bounds-checks
  untouched); `MmapFile()` (posix; nullptr→owned fallback on WASM/non-posix/
  failure), destructor munmaps on last shared_ptr. `ReadFile` prefers mmap;
  `CrateReadOptions::use_mmap` (default on) gates it. No in-heap crate copy for
  file loads; lazy values read straight from the mapping.
- **Phase 8 (paged secure snapshot)** — with `use_mmap = false`, file loads now
  capture the source once into an unlinked 0600 snapshot arena (one descriptor
  per snapshot directory, shared by every layer) and release the anonymous
  parse copy after the structural read
  (`CrateDataSource::PagedFileSnapshot`, `ReleaseParseBuffer`). Lazy arrays
  materialize by ranged `pread` from the sealed slice (`ReadRange` +
  `DecodeCrateArrayAt`), so retained RSS no longer equals the dependency
  closure. `base()` is null and `can_borrow()` false after release; view paths
  and the writer's verbatim pass-through fall back to materialize/re-encode.
  `CrateReadOptions::paged_snapshot` (default on) gates it. A failed or
  undersized snapshot destination degrades to the existing owned buffer; the
  source is still read exactly once and never aliased. WASM/non-posix keeps the
  owned path.
- **TimeSamples decoding** — crate TimeSamples now decode into per-property
  storage (was skipped on read); reads next- and pxr-written files.
- **Phase 7 (E2)** — layer offsets compose through the arc chain
  (ProcessArc + LayerOffset.Compose) and bake into time-sample times at
  BuildStage (CopyLocalOpinions remaps t -> offset + scale*t); USDA
  `(offset; scale)` captured by the parser; offset folded into the instance key.
- **Phase 9 (F2 + TSAN stress)** — LayerRegistry parses outside the lock
  (parallel layer load); a concurrent-payload-edit TSAN stress test guards the
  Phase-6 paths.
- **Phase 9 (F6)** — dropped the *recursive* engine mutex: the two
  re-entrantly-called methods (ComputePrimIndex, Invalidate) split into a public
  entry (locks once) + a lock-free `*_locked` worker; internal callers use the
  worker, so `api_mu_` is now a plain `std::mutex`.
  `LIGHTUSD_NEXT_RECURSIVE_LOCK` restores the recursive mutex as an escape
  hatch. TSAN-clean with threads on.
- **Phase 9 (F4, opt-in)** — `LIGHTUSD_NEXT_FINE_LOCKS` makes `api_mu_` a
  `std::shared_timed_mutex`: read-only queries + the ComputePrimIndex cache-hit
  fast path take a shared lock (concurrent), builds/writers take the exclusive
  lock; the write-lock double-check in `ComputePrimIndex_locked` keeps a
  contended prim built once (subsumes the in-flight-future dedup). Default and
  recursive policies keep READ==WRITE. Clean under ThreadSanitizer for test_pcp
  (incl. 8-thread×1000-iter concurrent queries on one shared cache).
  **Follow-up (post-roadmap review):** the READ_LOCK/WRITE_LOCK call sites and
  the `PcpMutex` policy selection (`pcp/cache-lock.hh`) had been written but
  `LIGHTUSD_NEXT_FINE_LOCKS` was never exposed as a CMake `option()` — every
  build silently fell back to the default single-exclusive-mutex policy no
  matter what a caller intended, i.e. T1 was still live in every actual build.
  Added the option to `src/next/CMakeLists.txt` (requires
  `LIGHTUSD_NEXT_ENABLE_THREAD`). Turning it on for the first time also
  surfaced two bugs the option's absence had kept latent: (1)
  `PropNameTable::is_frozen()` had no definition at all under
  `LIGHTUSD_ENABLE_THREAD` (only the non-threaded stub existed), so any
  threaded build referencing it failed to link -- fixed with an atomic-load
  read of the published snapshot pointer; (2) `test_tydra_next.cc`'s
  `TestNameTableFreezeStability` asserted that interning a new name while
  frozen unfreezes the table -- true under the *old* freeze() scheme, but
  freeze() was redesigned (see the F4 bullet's "lock-free reader" note above)
  to keep the snapshot published and let a miss fall through to the locked
  path instead, specifically so a concurrent lock-free reader is never
  disturbed. The test predates that redesign and was never caught because
  `is_frozen()` was a dead stub always returning `false`. Verified with a
  TSan build (clang, `LIGHTUSD_NEXT_ENABLE_THREAD=ON` +
  `LIGHTUSD_NEXT_FINE_LOCKS=ON`): 9 test binaries × 3 runs, 0 races; default
  (non-threaded) ctest suite still 35/35.
- **Phase 9 (F3)** — `layer_stacks`/`path_table` are `std::deque` (stable
  element addresses): a PrimIndex borrows raw pointers into these tables and
  resolves node sites lock-free (`SitePath`, used cross-thread), so a `vector`
  realloc from a concurrent build would dangle them. `SitePath`'s bounds check
  uses a per-PrimIndex size snapshot taken under the build lock, race-free
  against concurrent appends.
- **Phase 9 (F5)** — deterministic prototype assignment: the per-prim
  ComputePrimIndex cache and the parallel merge pick the lexicographically
  smallest member path as the prototype (`AssignPrototype` prefer_min mode), so
  the grouping is identical regardless of computation order/threads. BuildStage
  keeps first-in-namespace-order (single-pass, already deterministic); flatten
  output and the round-trip corpus are unchanged.

- **Phase 7 (S5 + E3)** — full cross-layer list-op representation. Arc
  qualifiers (prepend/append/delete/explicit) are stored per field in a lazily
  allocated `ArcListOpEdits` on the cold ext (the inline arc vectors stay as the
  within-spec effective list, so Phase 8.1's footprint is preserved); the USDA
  parser records them. With `CompositionOptions::apply_list_ops` (default on),
  `ExpandArcs` composes each arc field once across the site via `MergeArcField`
  (AOUSD SdfListOp weakest→strongest: explicit-replace / prepend / append /
  delete / dedup), in LIVRPS order. The USDA writer re-emits the authored
  qualifier (was always `prepend`), and the crate (USDC) writer/reader preserve
  it via a companion `<arc>_listOp` token[] field (the flat token[] still
  carries the within-spec effective list, so the change is additive). Tests:
  specialize chain, cross-layer delete + explicit-replace, USDA writer fidelity,
  USDC edit round-trip. doc/pcp.md has the implementation-status note +
  relocates scope. Remaining knob: whether to default `apply_list_ops` on after
  a corpus review.
- **Phase 10 (M9)** — `Cache::ComposePrim(path)` composes (and caches) one prim
  on first access, reusing BuildStageRec's SourcesForPath + ComposeInto step
  without materializing the whole stage; `ComposedChildNames(path)` for lazy
  descent; dropped by Invalidate/InvalidateLayer/SetLoadRules; fine-locks shared
  read fast path. Instancing/relocates stay stage-level (BuildStage
  authoritative). The lazy attribute entry point landed too:
  `EvalAttributeLazy(Cache&, Path, attr, opts)` resolves time samples / default
  via the shared `AttributeEval::EvalFromPrimSpec` and follows connections
  across lazily-composed prims -- no BuildStage.
- **Hardening (post-audit)** — fixed: spurious ext allocation in
  compose/flatten (`has_ext()` guard, restores the 8.1 footprint win); `issues_`
  lifecycle (cleared on Invalidate*/SetLoadRules; `GetCompositionIssues` returns
  by value to avoid a fine-locks dangling ref); `TokenPool` >4 GiB blob guard;
  `DropInstancing` orphans sibling `prototype_of` when a prototype is dropped.
- **Malformed USDC regression gate** — generated next-only crate fixtures now
  cover bad magic, truncated bootstrap, invalid TOC offsets/ranges, excessive
  section counts, missing required sections, allocation caps, and malformed
  FIELD/FIELDSET/SPECS payloads. These live alongside crash replay fixtures and
  assert returned errors rather than crashes or unbounded allocation.
- **Crate reader TU split** — public `CrateReader` API wrappers and source
  selection moved to `crate-reader-api.cc`; stage reconstruction moved to
  `crate-reader-stage.cc`; array decode/lazy-array probing moved to
  `crate-reader-arrays.cc`; scalar-vector decode moved to
  `crate-reader-vectors.cc`; scalar/non-array value dispatch stays in
  `crate-reader.cc` behind `crate-reader-internal.hh`.
- **ASCII parser TU split** — private parser implementation declarations moved
  to `ascii-parser-internal.hh`; stage/layer metadata parsing moved to
  `ascii-parser-metadata.cc`; namespaced-name parsing, skip helpers, property
  metadata, and parser diagnostics moved to `ascii-parser-utils.cc`;
  prim/property/relationship parsing moved to `ascii-parser-prim.cc`;
  `timeSamples` and variant parsing moved to dedicated TUs.
- **Writer/PCP helper split** — crate lazy-array pass-through policy moved to
  `crate-writer-passthrough.inc`; crate property/spec emission moved to
  `crate-writer-properties.inc`; PCP stage opinion fill, worker merge helpers,
  and arc list-op merge moved to focused includes.
- **Comprehensive generated USDC fixture** — `next_test_usdc_roundtrip` now
  builds a dense in-memory layer and writes/reads it through Crate. The fixture
  covers layer metadata, `customLayerData`, prim `customData`/`assetInfo`/
  `apiSchemas`, composition arcs, mesh arrays, per-property metadata,
  relationships, connection-flagged properties, Shader/Material connections,
  PointInstancer prototype/array data, `ids`/`invisibleIds`, and time samples.
- **Focused split-regression tests** — `next_test_usdc_writer` now pins
  `EncodeValue` fallback roundtrips for non-inline scalars/vectors/matrix,
  string/token/asset, and nested dictionaries; `next_test_pcp` now pins
  extracted-prototype collision handling, internal relationship remapping, and
  original-instance subtree orphaning.

`apply_list_ops` is now **default ON** (set false for legacy strong-first
concatenation). The decision rests on three checks:
 1. **Corpus-diff** (flatten each file with the flag off vs on): across the
    top-level corpus **no file differs or changes compose-success** — flipping
    is regression-free (the stock corpus has no file combining subLayers + arcs).
 2. **Positive coverage**: two dedicated fixtures
    (`tests/usda/composition/listop-xlayer-{delete,explicit}-000.usda`) differ
    exactly as intended under the review and nothing else does.
 3. **pxrUSD A/B**: flattening those cross-layer assets with OpenUSD `usdcat
    --flatten` and with next, `apply_list_ops=ON` **matches pxrUSD exactly**
    (the stronger layer's `delete` drops / a bare list replaces the weak
    sublayer's reference → only `fromB` survives), while the old OFF default was
    **non-conformant** (wrongly kept the deleted/replaced `fromA`).
So ON is the AOUSD-correct behavior, matches the reference implementation, and
is regression-free on the corpus — hence the default. (This review also
surfaced that the next USDA parser never read `subLayers` from text — a
double-consume bug, now fixed — and that the Release test build compiled out
`assert()`; tests now build with `-UNDEBUG`.) **The 10-phase roadmap is
complete.** The sections below are the original spec.

## Current Cleanup Backlog

The 10-phase roadmap is complete. The active work is now cleanup, validation,
and behavior-neutral split work:

1. **Docs/status hygiene** — keep this file, `src/next/README.md`, and
   `doc/memory-and-performance.md` aligned with landed behavior and benchmark
   deltas. When changing next test coverage, also update `doc/testing-cpp.md`.
2. **Standalone validation polish** — `scripts/run-next-checks.sh` is the
   canonical next smoke/regression entrypoint. Keep `next` outside the main
   regression gate until the assert-based tests are fully converted or otherwise
   hardened, even though the standalone CMake now forces asserts on for test
   targets.
3. **Behavior-neutral TU split** — the high-value split pass is complete:
   `crate-reader` API/source/stage/array/vector helpers, parser implementation/
   metadata/property/timeSamples/variant helpers, writer pass-through/property
   helpers, PCP helper/opinion-fill/merge/list-op includes, and value-parser
   helper include units are separated. Remaining cleanup is opportunistic only:
   smaller `EncodeValue` scalar/array subhelpers, deeper PCP instance-flatten
   extraction, or further parser arc/metadata refinements if future edits touch
   those sections.
4. **Memory observability** — make benchmark output stable enough to diff:
   struct sizes, `Layer::stats()`, RSS, lazy/eager clone RSS, crate write
   pass-through/reencode counts, and mmap-vs-heap attribution. Current
   `benchmark_next memstats` / `memstats-file` output covers these basics,
   including public `source_was_mmap` / `input_was_mmap` result fields.
5. **Format parity** — maintain the dense generated USDC fixture and focused
   malformed-crate fixtures. Continue adding next USDA/USDC writer parity tests
   and optional pxrUSD `usdcat` comparison when available.
6. **Remaining feature gaps** — keep broadening low-memory flatten coverage for
   mixed USDA/USDC/USDZ dependency graphs. Basic filesystem USDA sublayers and
   references are supported and covered. Compressed bool arrays are decoded
   (compressed-integral lanes canonicalized to true/false in
   `src/next/crate/crate-reader-arrays.cc`) and covered by regression tests.

Recently closed cleanup items:

- Blender/nanobind experimental bindings were deprecated out of the active next
  plan.
- PointInstancer support now includes typed accessors, extracted instance data,
  Tydra draw references, prototype material/transform bindings, validation, and
  opt-in transformed mesh duplication.
- Malformed USDC coverage now includes generated TOC/table/payload edge cases
  plus crash replay fixtures.
- A broad generated USDC roundtrip fixture now covers cross-feature read/write
  reconstruction instead of relying only on small single-feature tests.

Goals, in priority order:

1. **Secure** — no segfault/UB/stack-overflow on malformed or adversarial input
   (the module builds with `-fno-exceptions`; every failure must be a returned error).
2. **Low memory** — extend the lazy-ValueRep architecture; eliminate avoidable copies.
3. **Faster** — remove redundant work and quadratic algorithms in composition/flatten.
4. **Instancing** — complete the instance-key model and add consumer APIs.
5. **Lazy loading** — a real load-rules model for payloads (OpenUSD `UsdStageLoadRules`).
6. **Multithread-ready** — replace the single big lock with minimal, provably-safe
   locking; zero overhead when `LIGHTUSD_ENABLE_THREAD` is OFF.

Reference implementation for hardening/verification: `../OpenUSD/pxr/`
(Pcp prim indexing, `UsdStageLoadRules`, `Usd_InstanceCache`, `VtArray` CoW,
crate `_MmapFile`, `pcp/errors.h`). Spec reference: AOUSD Core §10 (see `doc/pcp.md`).

All file:line evidence below was verified by reading the code at HEAD `59801312`.
**It is a frozen historical snapshot, not current status** — every M/S/I item
and T1 are marked resolved in "Landed so far" above; only read this section
for *why* a change was made, never to decide whether something is still open.
A later review (post-roadmap) that treated M1/M3/M4/M5/M7 as open purely
because they appear in this table wasted a pass re-discovering they were
already fixed — check "Landed so far" first, always.

---

## 1. Verified problem inventory

### 1.1 Security / correctness

| # | Problem | Evidence |
|---|---------|----------|
| S1 | `BuildStageRec` has **no depth guard**. An ancestor self-reference (`def "A" { def "B" (references = </A>) {} }`) produces an infinitely-growing namespace: the per-prim-path cycle set starts fresh for every composed path, so cross-ancestral cycles are invisible → recursion until stack overflow. | `pcp/cache.cc:766` (no depth param), `cache.cc:580` (fresh cycle set per path) |
| S2 | Cycle detection copies a `std::set<std::string>` **per arc, per recursion level** — O(depth² × path-length) copies; the key is a concatenated string `stack-id + ":" + site`. | `pcp/cache.cc:466-469, 478-481, 529-531` |
| S3 | `SourcesForPath` recurses on the parent path — a pathological query path with thousands of components can blow the stack independently of S1. | `pcp/cache.cc:598-614` |
| S4 | Variant content layer-stacks are interned under a **pointer-derived key** `"variant:" + uintptr_t(vd)`. After `Invalidate` + recompose, a freed-and-reallocated `VariantData` can reuse an address while stale instance keys persist in `prototype_by_key` → **false prototype aliasing**. Also defeats prototype sharing across identical files (distinct `Layer` objects ⇒ distinct keys). | `pcp/cache.cc:518-520`; `DropInstancing` removes by prim only (`cache.cc:372`) |
| S5 | List-edit qualifiers (`prepend`/`append`/`delete`/`reorder`) are consumed and **discarded** by the USDA parser; arcs are flat `vector<string>` in `PrimSpecMeta`. `delete`/`explicit` are unrepresentable; `prepend` vs `append` cross-layer ordering is wrong per AOUSD §10.3.2. | `parser/ascii-parser.cc:577-584`; `layer/prim-spec.hh:90-93` |
| S6 | Layer offsets are never composed through arc chains (`ProcessArc` copies the parent offset only; sublayer `(offset/scale)` not parsed; value evaluation has no offset handling). | `pcp/cache.cc:446`; `layer/layer.hh:31`; `eval/attribute-eval.cc` |
| S7 | `deferred_payload_prims` is **mutated inside** `ExpandArcs` — a logically-const query writes shared state mid-expansion (blocks fine-grained threading); `UnloadPayload` invalidates without recomposing, so `HasDeferredPayload` returns stale `false` until something recomposes. | `pcp/cache.cc:556-559`; `cache.cc:1091`, `cache.cc:1059-1063` |
| S8 | No fuzz targets and no usd-wg corpus regression gate cover `src/next` (legacy has both; the corpus runner already accepts `--lusdcat PATH`). | `tests/fuzzer/usdcparser_fuzzmain.cc` (legacy-only); `tests/parse-asset-corpus.mjs:42` |

### 1.2 Memory / speed

| # | Problem | Evidence |
|---|---------|----------|
| M1 | `Value::copy_from` **deep-copies array vectors** for every non-lazy array type. Every compose/flatten/Clone path pays it: all USDA input, token arrays, and eager crate types. Lazy crate arrays already copy as a refcount bump. | `types/value.cc:631-658`; callers `composition/composition.cc:171,206`, `pcp/cache.cc:651,675`, `layer/prim-spec.cc:143,340-389` |
| M2 | `FindSpecs` is re-resolved 3–5× for the same (stack, site) per composed prim, each call allocating a fresh `vector<SpecRef>` with a `std::string layer_id` copy per hit. | `pcp/cache.cc:217-231`; callers at `cache.cc:243,305,319,663,728` |
| M3 | pcp hot containers are string-keyed (`index_cache`, `sources_cache`, `Site{string,string}`, payload sets, prototype maps) although an `InternPath` u32 table already exists and is used only for `CompNode::site_path_idx`. | `pcp/cache.cc:96-121` |
| M4 | `PrimSpec` carries ~660 B inline (`PrimSpecMeta` ≈ 350 B: 3 strings + 8 vectors + variant machinery; three `unordered_map` members) plus **2 mandatory heap allocations** per prim (`ValueStorage` + `TimeSampleStorage`) even when empty. `ValueStorage::data_`/`allocate()`/`raw()` is dead code (`store()` only uses `values_`). | `layer/prim-spec.hh:78-108,431-444`; `prim-spec.cc:119-147,319-333` |
| M5 | `Compositor::GraftSubtree` scans **all prims of the source layer** with string-prefix compares per graft call → O(layer_prims × graft_count) on the flatten path (the one WASM uses). | `composition/composition.cc:344-364`; call sites `:251,271,303,331,413` |
| M6 | Crate reading copies the whole file to one heap `std::string`; `Read(ptr,len)` double-copies; **no mmap** path exists in src/next. | `crate/crate-reader.cc:886-938`; `crate/crate-data-source.hh:68` |
| M7 | Tokens: LZ4 blob → one `std::string` per token, then re-copied into Values and into `PropNameTable` (3 copies of the token table). | `crate/crate-reader.cc:1058-1117, 275-280, 864-875` |
| M8 | Lazy array coverage now includes Vec4f/Quatf, half arrays, Vec2-4d/Quatd/Matrix2-4d, and crate TimeSamples sample values. Remaining risk is regressions in unsupported/unknown crate array layouts, which must keep `block_len=0` so writer pass-through never copies guessed bytes. | `crate/crate-reader.cc`, `crate/crate-data-source.cc`, `tests/next/test_lazy_array.cc`, `tests/next/test_crate_alloc_guard.cc` |
| M9 | `BuildStage` materializes the entire composed stage eagerly; read-only consumers pay O(scene) RSS. | `pcp/cache.cc:766-835` |

### 1.3 Threading

| # | Problem | Evidence |
|---|---------|----------|
| T1 | ~~One `std::recursive_mutex api_mu_` serializes every public cache entry point when `LIGHTUSD_ENABLE_THREAD` is ON.~~ **CLOSED** — Phase 9 F6 dropped the recursive engine mutex (plain `std::mutex` + lock-free `*_locked` internals); Phase 9 F4 added the `LIGHTUSD_NEXT_FINE_LOCKS` shared/exclusive policy on top, and the CMake option to actually turn it on landed in the post-roadmap follow-up (see the F4 bullet above) — until then the option existed in code but was unreachable, so every real build still paid the T1 cost regardless of intent. | `pcp/cache.cc:24,79` (historical) |
| T2 | `LayerRegistry` holds its mutex **across file parse**, so the "parallel" layer prefetch in `PrewarmPrimIndices` parses one file at a time. | `pcp/layer-registry.cc:69-92`; `cache.cc:936-951` — **STATUS UNCLEAR**: Phase 9 F2 ("LayerRegistry parses outside the lock") claims this closed; not re-verified line-by-line in the post-roadmap review, worth a fresh read before relying on it. |
| T3 | Race inventory if the big lock were removed: `path_table`/interning, `layer_stacks` vector reallocation under readers, `index_cache`/`sources_cache`, `site_to_indices`, deferred-payload set mutation mid-expansion (S7), instance maps, registry counters. | `pcp/cache.cc:92-121, 556-559` — covered by Phase 9 F3 (deque-stable addressing) and F5 (deterministic prototype assignment); TSan-clean per the F4 bullet, but see the F4 follow-up note: that TSan pass predates the CMake-wiring fix, so it never actually ran the fine-locks code path either. Re-verified in the post-roadmap review (9 binaries × 3 runs, 0 races) — see F4 bullet. |

### 1.4 Instancing

| # | Problem | Evidence |
|---|---------|----------|
| I1 | Instance key omits accumulated variant selections and layer offsets; variant content contributes a pointer-derived stack id (S4). OpenUSD's `PcpInstanceKey` includes all authored variant selections from every node; `Usd_InstanceKey` additionally hashes load rules relative to the instance. | `pcp/cache.cc:315-336`; `pxr/usd/pcp/instanceKey.cpp:54-68`, `pxr/usd/usd/instanceKey.cpp:42-106` |
| I2 | Instanceable predicate = "any spec authored `instanceable=true`" instead of strongest-opinion; authored `false` is indistinguishable from unauthored (`bool`, not tri-state). | `pcp/cache.cc:302-310`; `layer/prim-spec.hh` |
| I3 | No instance↔prototype path-translation API for consumers (Tydra); nested instancing structurally works but is untested. | `pcp/cache.hh:67-101` |
| I4 | PointInstancer typed accessors, transform/mask-style data extraction, prototype mesh bindings, lightweight Tydra draw refs, prototype-relative transforms, per-draw material IDs, unresolved-prototype diagnostics, bounds-checked draw view resolution, O(1) per-instancer draw ranges, draw/prototype binding validators, and opt-in transformed mesh duplication now exist. | `src/next/schema/geom-point-instancer.hh`; `src/tydra/next/render-extract.hh`; `src/tydra/next/render-data.hh` |

---

## 2. Phased plan

Each phase is independently landable. Ordering rationale: security first (it
restructures `ProcessArc`/`ExpandArcs` signatures that later phases build on),
then the safety nets (fuzz/corpus), then memory/speed, then semantics, then
concurrency last (after mid-query mutations are gone).

### Phase 0 — Plan + baselines (this doc)

- Write this document.
- Add a `mem_stats` utility (extend `benchmark_next` or a tiny new target) printing
  `sizeof(Value/PrimSpec/PrimSpecMeta/CompNode)` and `Layer::memory_usage()`/`stats()`
  for a 100k-prim synthetic layer.
- Add a pcp compose wall-time benchmark (N references × M prims; nothing currently
  times `Cache::BuildStage`).
- Capture baselines into `doc/memory-and-performance.md`:
  `bench_lazy_mem` (`gen`/`eager`/`lazy`/`genmany`), `benchmark_next`,
  WASM flatten memlog (`-DLIGHTUSD_FLATTEN_MEMLOG`, `pipeline/flatten.cc:29-35`),
  massif on a large `.usdc`.

### Phase 1 — Security: recursion & cycle hardening (S1–S3 + quick wins)

OpenUSD analogue: `pxr/usd/pcp/primIndex_StackFrame.h` — a linked chain of stack
frames walked to detect site re-entry across recursive index builds.

1. Depth-guard `BuildStageRec` (thread a `depth` param, fail with error at
   `options.max_depth`) and the stage child traversals (`stage/stage.cc:187`).
2. Replace the per-arc copied cycle sets with a **stack-frame chain**:
   `struct ExpansionFrame { uint32_t stack_idx; const std::string* site; const ExpansionFrame* prev; }`
   threaded through `SourcesForPath → ExpandArcs → ProcessArc`. Reproduces the
   current in-expansion cycle semantics over the chain, *and* detects ancestral
   re-entry (S1's real fix). Emit an ArcCycle error and drop the arc
   (continue-past-errors, AOUSD §10.6). This is simultaneously the S2 memory/CPU
   fix — one restructure, not two.
3. Convert `SourcesForPath` parent recursion to an iterative root→leaf loop (S3).
4. Parser/crate recursion audit: USDA nested-prim depth cap (mirror crate's
   `max_path_depth`); cross-check crate bounds against `pxr/usd/sdf/crateFile.cpp`
   patterns (ValueRep payload-offset validation, LZ4 compressed-size sanity).
   Document the constraint that any future recursive value type (dictionaries)
   must carry a depth cap.
5. Quick memory wins riding the same PR series: delete dead
   `ValueStorage::data_/allocate/raw/reserve`; lazily allocate `time_samples_`
   on first `add_time_sample` (accessors already null-check).

Tests (all must error, not crash; run under ASAN): ancestor self-reference,
mutual A↔B cross-file reference cycle, self-payload, variant content referencing
its host, 1000-deep authored hierarchy, reference chain at exactly `max_depth`.

### Phase 2 — Fuzzers + corpus gate for next (S8)

- `tests/fuzzer/next_usdc_fuzzmain.cc` — prepend `PXR-USDC` magic (same trick as
  the legacy harness), feed next's USDC reader with tight `CrateReadOptions` caps
  so OOM ≠ finding.
- `tests/fuzzer/next_usda_fuzzmain.cc` — USDA reader path.
- `tests/fuzzer/next_compose_fuzzmain.cc` — split input into length-prefixed
  pseudo-layers, register via `Cache::PreloadLayer` + stub resolver,
  `ComposeStageFromLayer` with `max_depth=64`. The only harness exercising Phase 1.
- New `LIGHTUSD_NEXT_BUILD_FUZZERS` option in `src/next/CMakeLists.txt`
  (clang-only, `-fsanitize=fuzzer,address`).
- `next_usdcat` CLI (~100 LOC: load + compose + print) and a `corpus-parse-next`
  CTest entry mirroring `corpus-parse-native` (commit `88412f30`), gated on
  `USD_WG_ASSETS_DIR` with graceful SKIP; `--max-fail 0` parse, `--max-fail N`
  flatten while composition gaps remain.
- Commit crash regressions as fixed-input unit tests in `tests/next/`.

### Phase 3 — CoW array storage in Value (M1; top memory win)

`VtArray::_DetachIfNotUnique` analogue: store `std::shared_ptr<XxxArrayStorage>`
in the SBO slot instead of a raw owning pointer. Copy = atomic refcount bump.
Non-const accessors (`as_float_array()` mutable, `raw_data()` mutable) detach
(clone when `use_count() > 1`) **and `mark_dirty()`** — the crate writer's verbatim
block pass-through keys off `is_dirty()`, so detach-without-dirty would corrupt
output. `move_from` and `MakeXxxArray(vector&&)` unchanged.

Gate: `bench_lazy_mem eager` should approach `lazy` for the clone portion;
`next_test_usdc_roundtrip` / `next_test_usdcat_roundtrip` green.

### Phase 4 — pcp speed: memoization + interned keys + graft fix (M2, M3, M5)

1. Memoize `FindSpecs` per `(stack_idx, interned site-path id)`:
   `unordered_map<uint64_t, vector<SpecRefIdx>>` where `SpecRefIdx =
   {const PrimSpec*, uint16 layer_idx}` (derive `layer_id` from
   `LayerStack::layer_identifiers[layer_idx]` instead of copying). Reuse across
   `AnyAuthors` / `ComputePrimIndex` / `ComposeInto` / instance-key computation.
   Invalidate together with `sources_cache` (rides existing `Invalidate` paths).
2. Convert hot pcp maps to interned u32 ids using the existing `InternPath` table:
   `Site` → `{uint32 layer_id_idx, uint32 path_idx}`; `index_cache`/`sources_cache`/
   payload/prototype maps keyed by interned root-path id; `Src::site` → u32.
   `MergeWorkerIndex` already remaps interned indices, so the worker path extends
   naturally.
3. `GraftSubtree`: walk the source subtree via `child_indices_` from
   `prim_at_path(src_root)` instead of a full-layer prefix scan. **Preserve the
   grafted-prim output order** (crate output determinism; roundtrip tests gate).
   `arc_resolved_`/`graft_paths_` `std::set<string>` → `unordered_set` / interned ids.

### Phase 5 — Instancing completeness (S4, I1–I4)

1. Instance key correctness (OpenUSD `PcpInstanceKey`/`Usd_InstanceKey` as truth):
   - Key variant content stacks by `(owning stack identifier, site, variantSet,
     variantName)` — kills the pointer key (S4).
   - Include the full accumulated variant-selection map and `Src.offset` in the key.
   - Deferred payloads already contribute no `Src`, so load state correctly splits
     keys — add a test (load one of a pair → prototypes split; load both → re-merge).
   - Strongest-opinion `instanceable` resolution; `instanceable` becomes tri-state
     in `PrimSpecMeta` (parser change: authored `false` ≠ unauthored).
2. Path translation API for consumers (Tydra): **DONE** —
   `Cache::TranslatePathToPrototype(path)` (longest registered instance prefix,
   recurse for nesting, iteration-capped) returns empty when `path` is under no
   instance; `TranslatePathFromPrototype(proto_path, instance_root)` is the
   single-instance inverse. `src/next/pcp/cache.{hh,cc}`.
3. Nested-instancing tests: **DONE** — `test_pcp.cc:test_path_translation`
   covers prototype-within-prototype (two-level rewrite through both prototype
   roots), recompose identity consistency, and empty-on-no-instance exclusion.
4. Port the PointInstancer compute API: **DONE** —
   `src/next/schema/geom-point-instancer.{hh,cc}` provides `GetPrototypes`,
   the instance arrays, `ComputeInstanceTransforms(time)`, and
   `ComputeMaskAtTime(time)` (per-instance visibility from `invisibleIds` +
   `inactiveIds`, id-mapped via `ids`); same preliminary limitations (velocities
   ignored, orientations held). Bounds/length mismatches return empties, not UB.
   Covered by `tests/next/test_schemas.cc:test_point_instancer_schema`.
   Security: `protoIndices[i] < prototypes.size()` and array-length mismatches →
   error, not UB. Shared fixture `tests/usda/pointinstancer-expand-001.usda`,
   numeric parity with the legacy test asserted from `tests/next/test_pointinstancer.cc`.

Gate: the serial/parallel prototype-grouping parity test re-run (key format changes
which prims share prototypes).

### Phase 6 — Lazy payload LoadRules (S7 + load-rules model)

OpenUSD analogue: `pxr/usd/usd/stageLoadRules.h` (sorted `(path, All|Only|None)`
rules with ancestor/descendant resolution) + `PcpCache::_includedPayloads`.

1. New `pcp/load-rules.{hh,cc}`: exception-free `LoadRules` port
   (`GetEffectiveRuleForPath` via binary search + ancestor scan, `Minimize()`).
   ~200 LOC, independently unit-tested against the OpenUSD truth table.
2. Replace `payloads_force_loaded/_unloaded` with one `LoadRules`;
   `ShouldLoadPayload` falls back to `payload_policy`/`load_payloads` when no rule
   matches. `LoadPayload(p)` ≙ `AddRule(p, AllRule)` so existing tests pass unchanged.
3. `LoadPayload(path, WithDescendants|WithoutDescendants)` / `UnloadPayload` /
   `SetLoadRules(rules)`; each computes the changed-path set and invalidates only
   those subtrees (rule-delta-driven recompose granularity).
4. Deferred-set lifecycle fix (S7): `ExpandArcs` returns discovered deferred sites;
   `ComputePrimIndex` commits them. `UnloadPayload` recomposes so
   `HasDeferredPayload` is immediately truthful. **Prerequisite for Phase 9.**
5. Payload-on-instance semantics, tested: loading a payload on instance I2 whose
   key-equal sibling I1 is the prototype splits I2 from the group; unload re-merges.

### Phase 7 — Composition correctness: list-edits, offsets, typed errors (S5, S6)

1. **ListOps (S5)**: `PrimSpecMeta` arc fields become
   `ListOp { explicit_, prepended, appended, deleted, ordered; bool is_explicit; }`.
   Parser routes the already-tokenized qualifier; bare `references = [...]` ⇒ explicit.
   Crate reader maps crate ListOp fields. New `ApplyListOps(strong→weak)` helper
   applied **per layer stack** in `ExpandArcs` (weakest-first per AOUSD §10.3.2) —
   also fixes the latent double-expansion of an arc authored identically in two
   sublayers. Behind `CompositionOptions::apply_list_ops = true`; review the corpus
   flatten diff once (Phase 2 gate is the safety net).
2. **Layer offsets (S6)**: parse sublayer `(offset = ..; scale = ..)` into layer
   metadata and arc offsets into `CompositionArc`; compose `offset·scale` through
   `ProcessArc`; include in the instance key (Phase 5 reserves the slot); apply at
   value resolution — BuildStage path first (per-arc-chain offset, fixing the
   one-offset-per-prim approximation), `attribute-eval` time remap
   `t → (t − offset)/scale` after.
3. **Specialize corners (E3)**: tests for specialize chains (A⇒B⇒C weakest-of-all),
   implied-inherit-discovered-under-specialize containment (audit
   `cache.cc:459-472` target routing), variant-internal arcs. Document relocates
   scope (same-parent rename only) in `doc/pcp.md`.
4. **Typed errors (E4)**: `enum class ErrorCode : uint8_t { ArcCycle, SublayerCycle,
   MaxDepthExceeded, InvalidAssetPath, UnresolvedPrimPath, IndexCapacityExceeded,
   InvalidVariantSelection, InvalidReferenceOffset, ... }` +
   `struct CompositionIssue { ErrorCode code; std::string site, message; }`
   accumulated on `Impl`; the existing `std::string *err` is rendered from it
   (no API break); tests assert on codes instead of message substrings.

### Phase 8 — Memory: PrimSpec footprint, token pool, mmap, lazy coverage (M4, M6–M8)

1. `PrimSpecMeta` split: keep `active/hidden/instanceable` + the 4 arc fields
   inline; move `variantSets`, `variantSelections`, `relocates`, `apiSchemas`,
   `doc`, `comment`, `instance_prototype` behind a lazily-allocated
   `unique_ptr<PrimSpecMetaExt>`. Relationships keyed by `PropNameId`
   (precedent: `connections_` already is). `Layer::finalize()` shrink_to_fit on
   `prims_`, `child_indices_`, slots. Expect 2–3× per-prim fixed-cost reduction
   (most prims have no relationships/variants/timesamples).
2. Token pool (TfToken-lite): `tokens_` = `{std::string blob; std::vector<uint32_t>
   offsets;}` — no per-token string; intern property names straight from the pool;
   optionally a token-index array storage (`vector<uint32> ids` + pool ref)
   materialized on demand.
3. mmap-backed `CrateDataSource` (posix first; OpenUSD `_MmapFile` analogue):
   second backing mode `{base, size, MmapHandle}`; `CrateReader::ReadFromFile`
   picks mmap when available. `StreamReader` is already `(base,size)` +
   bounds-checked, so the reader body is untouched; lazy values hold
   `shared_ptr<CrateDataSource>` so lifetime is already correct. WASM keeps the
   owned-string path. Bounds checks must remain (hostile concurrent writer).
4. Preserve the expanded lazy-array surface (Vec4f/Quatf, half arrays,
   Vec2-4d/Quatd/Matrix2-4d, and TimeSamples sample values) with regression
   tests. String/token arrays stay eager because they require table remapping;
   unsupported compressed/unknown layouts must remain non-pass-through
   (`block_len=0`) until their exact byte layout is implemented.

### Phase 9 — Multithread-readiness: minimal locking (T1–T3; land last)

No TBB; C++14 only; everything behind `LIGHTUSD_ENABLE_THREAD` macros that compile
to nothing when OFF. Sub-steps independently landable with the big lock as fallback
(`LIGHTUSD_NEXT_FINE_LOCKS` escape hatch during bring-up).

1. **LayerRegistry parallel parse (T2; biggest practical win, independent):**
   per-key `map<string, shared_future<shared_ptr<Layer>>>` under a small mutex;
   first requester inserts a promise and parses **outside** the lock; others wait
   on the future.
2. Enforce immutable-after-publish layers: registry hands out
   `shared_ptr<const Layer>`; audit that the pcp path never mutates fetched layers.
3. Append-only shared tables: `layer_stacks`/`path_table` → `std::deque` (stable
   addresses), small append mutex, `std::atomic<uint32_t>` published-size with
   release/acquire.
4. Index/sources maps: `std::shared_timed_mutex` (shared lookup, unique insert) +
   in-flight `unordered_map<id, shared_future<const PrimIndex*>>` so concurrent
   requests for the same prim build once.
5. Instance registration: builds return `(instanceable, key)`; a small locked
   `AssignPrototype` commits in deterministic publication order (generalizes the
   existing deferred mode, `cache.cc:362-368`).
6. Writers (`Invalidate`/`InvalidateLayer`/`LoadPayload`/`UnloadPayload`/`BuildStage`)
   stay exclusive; drop the *recursive* mutex by delegating public methods to
   lock-free `*_locked` internals.
7. TSAN stress tests in `tests/next/test_pcp_parallel.cc` (build/next-tsan):
   contended same-prim `ComputePrimIndex`; queries racing `Load/UnloadPayload` on
   disjoint subtrees; concurrent first-touch of one referenced layer; concurrent
   key-equal instance registration (prototype determinism); `Invalidate` racing
   queries. **Opt-in only — TSAN tests are never enabled by default (project policy).**

### Phase 10 — Exploratory: prim-index-backed lazy Stage (M9)

`ComposedPrimCache` inside `pcp::Cache`: per-prim composed spec built on **first
access** (reusing `ComposeInto`) instead of eager whole-stage `BuildStage` for
read-only consumers; `attribute-eval` gains a `(pcp::Cache*, Path)` entry point.
`BuildStage` remains for the flatten/write pipeline. Highest risk (new API,
invalidation coupling, thread-safety) — only after Phases 3, 4, and 9.

---

## 3. Dependency summary

| Phase | Depends on | Notes |
|-------|-----------|-------|
| 0 | — | baselines before any change |
| 1 | — | restructures ProcessArc/ExpandArcs signatures used later |
| 2 | 1 | so fuzzing doesn't trivially re-find Phase 1 holes |
| 3 | — | shared_ptr refcount is atomic → thread-friendly by construction |
| 4 | 1 (frame chain) | MergeWorkerIndex remap pattern reused |
| 5 | 4 helpful (interned ids in keys) | key change gated by serial/parallel parity |
| 6 | 5 (key/load-state tests) | D-step 4 is a Phase-9 prerequisite |
| 7 | 2 (corpus diff net) | ListOps changes composed output |
| 8 | 3 (CoW), 2 (fuzz for mmap/token pool) | |
| 9 | 6.4, 5.1 (mutation-free queries) | sub-steps landable individually |
| 10 | 3, 4, 9 | exploratory |

## 4. Measurement & verification protocol

- Per phase: `cmake --build <next-build> -j16 && ctest --output-on-failure`
  (all `next_test_*`).
- Memory: `bench_lazy_mem` (4 numbers), `mem_stats` sizeofs, WASM flatten memlog
  (`after-read` / `after-compose` / `after-write`), massif — deltas vs the Phase-0
  table recorded in `doc/memory-and-performance.md`.
- Speed: new pcp compose bench + `benchmark_next`.
- Robustness: ASAN build for Phase-1 crash tests; each fuzzer ≥1h locally after
  Phase 2; `corpus-parse-next` with `USD_WG_ASSETS_DIR` set.
- Threading: `build/next-tsan` with `LIGHTUSD_NEXT_ENABLE_THREAD=ON`; stress tests opt-in.
- Determinism: `next_test_usdcat_roundtrip` + serial/parallel grouping parity guard
  Phases 4/5/9.

## 5. Critical files

- `src/next/pcp/cache.{hh,cc}` — Phases 1, 4, 5, 6, 9
- `src/next/types/value.{hh,cc}` — Phase 3
- `src/next/composition/composition.cc` — Phases 1, 4, 7
- `src/next/layer/prim-spec.{hh,cc}` — Phases 1, 7 (ListOp), 8
- `src/next/parser/ascii-parser.cc` — Phases 1, 7
- `src/next/crate/crate-reader.cc`, `crate/crate-data-source.hh` — Phases 2, 8
- `src/next/pcp/layer-registry.cc` — Phase 9
- New: `src/next/schema/geom-point-instancer.{hh,cc}`, `src/next/pcp/load-rules.{hh,cc}`,
  `tests/fuzzer/next_*`, `tests/next/test_pointinstancer.cc`
- `src/next/CMakeLists.txt`, top-level `CMakeLists.txt` (corpus gate)

---

## Phase 2 results (fuzzers + corpus gate)

Three libFuzzer harnesses (`LIGHTUSD_NEXT_BUILD_FUZZERS=ON`, clang) +
`next_usdcat` CLI + `next_corpus_parse` CTest gate. The fuzzers found **7
crashes** in the first minutes, all now fixed and pinned by
`tests/next/test_crash_regressions.cc` (replayed under ASAN/UBSAN):

1. `value-parser.cc:508` — `std::string(GetTypeName(id))` with `id` lacking
   TypeInfo → `std::string(nullptr)` abort. (USDA + compose.)
2. `crate-reader.cc` ReadFields token-indices — `memcpy(dst+8, v.data(), 0)`
   with empty vector → null-arg UB.
3. `crate-format.cc:457` DecodeIntegers — attacker-controlled `common_bytes`
   (up to 255) → out-of-range `<< (i*8)` shift UB. Reject `> 4`.
4. `stream-reader.hh:90` `read()` — zero-count `memcpy` with null src/dst UB
   (both overloads).
5. `crate-reader.cc` ReadFields value-reps — zero-count `memcpy` after decode.
6. `crate-reader.cc` ReadTokens — unterminated token blob → `strlen` reads past
   the decompressed buffer (heap-buffer-overflow). Now `memchr`-bounded.
7. (re-found #1 via the compose path before relink.)

Post-fix soak (clang, ASAN+UBSAN): USDA 590k runs, USDC ~bil runs across
sessions, compose 1.86M runs — all clean.

Corpus gate (`next_corpus_parse`, usd-wg/assets, 280 files): **0 FAIL /
0 CRASH** — every corpus file now loads (a few USDC files still report
warnings on unsupported binary value types). Registered as a regression
ratchet (`--max-fail 3`, `src/next/CMakeLists.txt`); the ratchet replaced
the original `--max-fail 220` baseline (15 PASS / 46 WARN / 219 FAIL) as the
list-op qualifier, dictionary-metadata, and compressed/extra array-type gaps
closed. Ratchet down further as remaining warnings close.

# Thread-Safety & Data-Race Notes

Scope: concurrency in the `lightusd::pcp::Cache` engine (`src/pcp/`) and the
shared code its optional multithreaded build path touches. This is the working
record of what was audited, what was fixed, what remains, and how to verify.

Context: `pcp::Cache::PrewarmPrimIndices()` / `BuildStage()` can build
independent prim indices on worker threads when `CacheOptions::num_threads != 1`
and `LIGHTUSD_ENABLE_THREAD` is compiled in (never on wasm). Each worker runs
`Cache::Impl::BuildEntry() -> composition_graph::PrimIndexBuilder::Build()`
against the **shared** composited root layer and the registry-shared referenced
layers. That sharing is where data races live.

Audit method: read every shared object the workers touch concurrently, plus a
ThreadSanitizer run of the pcp tests and the full unit suite (see *Reproducing*
below).

---

## Fixed (for reference)

### Layer::find_primspec_at lookup cache — **FIXED**
`Layer::find_primspec_at()` is `const` but mutates a lazy lookup cache
(`LayerImpl::_primspec_path_cache` / `_dirty`, `src/layer.cc`). Parallel pcp
builds call it concurrently on the shared root layer and on registry-shared
referenced layers → unsynchronized `std::map` writes = UB. ThreadSanitizer
reported 16 races at `src/layer.cc` before the fix, 0 after.

Fix: a gated `std::shared_ptr<std::mutex> LayerImpl::_cache_mu` (`src/layer.cc`
~L246) guards only the cache read/write in `find_primspec_at()` (`cache_lock`
at ~L476, released for the lock-free `_prim_specs` tree walk at ~L494, re-taken
for the insert at ~L505). Gated on `LIGHTUSD_ENABLE_THREAD`, so non-threaded /
wasm builds are byte-for-byte unaffected. `shared_ptr` keeps `Layer`
copyable/movable. Covered by `pcp_singlethread_vs_multithread_identical_test`
and `pcp_mt_shared_reference_test`.

### TaskQueue / TaskQueueFunc payload publication — **FIXED**
The lock-free ring claimed a slot via CAS on `_write_pos`/`_read_pos` and only
*then* stored/loaded the `TaskItem` payload (`src/task-queue.hh` Push/Pop), so
the payload access was unordered w.r.t. the position publish → a consumer could
read a slot before the producer wrote it. TSan flagged this in
`task_queue_multithreaded_test`. (pcp itself never uses `TaskQueue`; the
producer here is `src/prim-pprint-parallel.cc`.)

Fix: rewrote both queues as a Dmitry Vyukov bounded MPMC queue. Each ring slot
carries a `std::atomic<uint64_t> seq`; the producer stores the payload then
`seq.store(release)`, and the consumer `seq.load(acquire)` before reading — so
publication is correctly ordered. Single `std::atomic`-based implementation
(replaces the hand-rolled `__atomic_*` / mutex-fallback dual path); public API
(`Push`/`Pop`/`Size`/`Empty`/`Capacity`/`Clear`) unchanged. The i386 64-bit
atomic test skip stays (std::atomic is still correct there, just not lock-free).
TSan-clean in `task_queue_multithreaded_test`.

### Path::variant_part() / element_name() mutable buffers — **FIXED**
Both `const` accessors previously wrote `mutable` string buffers, racing if
called on a shared `Path` from two threads.

Fix: `Path` now stores the element as an eagerly-computed slice of `_full`
(`_elem_off`/`_elem_len`, set at construction / `append_*`), and `element_name()`
returns a `tstring_view` over that slice — a lock-free read with no `mutable`
state. `prim_part()`/`prop_part()` likewise return views into `_full`; the
formatted variant string is built and returned by value (no cached `mutable`
member). `Path::operator==` compares the owning full path and never depended on
any lazy side-effect.

### Layer::check_unresolved_*() mutable flags — **FIXED**
The six `const` compute-and-cache methods (`check_unresolved_references` … and
`check_over_primspec`) and their `has_unresolved_*()` getters touched
`mutable bool _has_*` flags without synchronization. Now each write and each
getter read is wrapped in a `std::lock_guard` on the gated `LayerImpl::_cache_mu`
(`#if defined(LIGHTUSD_ENABLE_THREAD)`), reusing the same mutex as the lookup
cache. Zero overhead when threads are off. (`_has_class_primspec` is fixed at
construction — there is no `check_class_primspec()` that recomputes it — so its
getter is intentionally left unguarded.)

### Layer copy carries stale cache pointers + shared mutex — **FIXED**
`Layer::Layer(const Layer&)` / `operator=` copied `*other._impl` wholesale,
which (a) copied `_primspec_path_cache` (`const PrimSpec*` into the *source's*
tree) and (b) copied the `shared_ptr<mutex> _cache_mu` **by value**, so a copy
*shared the source's mutex*. Both copy paths now reset the lookup cache on the
new impl: `_dirty = true`, `_primspec_path_cache.clear()`, and (gated) a fresh
`_cache_mu = std::make_shared<std::mutex>()`.

### Callback thread-safety contract — **DOCUMENTED**
`CompositionGraphOptions::payload_policy` and `::fileformats` (in
`src/composition-graph.hh`) and `pcp::CacheOptions::composition` /
`PrewarmPrimIndices` / `BuildStage` (in `src/pcp/cache.hh`) now carry doc
comments stating that, with `num_threads != 1`, these callbacks run concurrently
on worker threads and must be thread-safe. No code change.

---

## Fixed — Round 2 (repo-wide audit of lightusd + tydra)

A second, broader audit swept every concurrency entry point and shared-mutable
candidate across `src/` (including `src/tydra/`). The findings below were fixed;
everything else is recorded under *Confirmed safe* / *Latent — by design*.

### Stage::GetPrimAtPath / find_prim_by_prim_id lazy caches + copy — **FIXED**
`Stage::GetPrimAtPath()` and `find_prim_by_prim_id()` are `const` but clear+populate
lazy caches `mutable HashMap _prim_path_cache` / `_prim_id_cache` and the
`_dirty` / `_prim_id_dirty` flags (`src/stage.cc`). `lightusd::HashMap`
(`src/tiny-hashmap.hh`) has no internal locking, so concurrent reads on a shared
`Stage` race exactly like the `Layer::find_primspec_at` case. `Stage` is also
**copyable** and the cached `const Prim*` point into the source's `_root_nodes`,
so a copy carried stale pointers.

Fix: a gated `std::shared_ptr<std::mutex> Stage::_cache_mu` (`src/stage.hh`)
guards the cache read/write in both methods with the same lock → unlock for the
lock-free `_root_nodes` walk → relock-for-insert pattern as `Layer`. The
user-defined copy ctor / `operator=` reset both caches and install a fresh mutex
(`_dirty=_prim_id_dirty=true`, `clear()`, gated `make_shared`). Move stays
`=default`. Covered by `stage_concurrent_find_prim_test` (8 threads × shared
Stage, TSan-clean).

### Xformable::GetLocalMatrix lazy matrix cache — **FIXED (cache removed)**
`GetLocalMatrix()` (`src/xform.hh`) is `const` but cached into `mutable
value::matrix4d _matrix` / `mutable bool _dirty`, racing if one `Xformable` is
evaluated from two threads. `Xformable` is embedded by value in every xformable
prim (potentially millions), so a per-object mutex was rejected on memory
grounds. Usage analysis showed the cache is barely useful (`set_dirty()` has no
callers; the 2 `GetLocalMatrix` callers pass a time and the cache only applied to
default-time). Fix: compute and return by value on every call; the `_matrix` /
`_dirty` members are removed and `set_dirty(bool)` is now a deprecated no-op shim
(kept for source compatibility). Race removed at the source, zero overhead. To
parallelize over one logical Xformable, give each worker its own (cheap) copy.

### Prim::get_child_indices_from_primChildren lazy cache — **FIXED (cache removed)**
This `const` accessor (`src/prim-types.cc`) cached into `mutable
_primChildrenIndices` / `_child_dirty` / `_primChildrenIndicesIsValid`
(`src/core/prim.hh`) and is effectively dead (no callers outside its own TU). Fix:
returns `std::vector<int64_t>` **by value**, computed locally; the three mutable
members and the `_child_dirty=true` writes in `add_child`/`replace_child` are
removed. `force_update` is retained as a no-op for signature compatibility.

---

## Fixed — Round 3 (process-wide state + value finalization)

A third sweep targeted process-wide shared mutable state and remaining lazy-const
mutation that a real multithread user would hit (loading/writing files, reading
animated attributes from several threads) — none of which the pcp-focused tests
exercised.

### TimeSamples::get_samples() unified→generic materialization — **FIXED (audit follow-up)**
A re-audit of the round-3 commit found the eager-finalize was *incomplete*.
`TimeSamples::update()` only sorts; it does not build the generic `_samples`
vector. USDC numeric attributes use unified (`_times`/`_data`) storage with
`_samples` empty, so the first generic read — `get_samples()`, reached by
`get_value()` and array/non-binary `get<T>()` (`src/timesamples-eval.cc`) —
lazily materialized `mutable _samples` under `const`. Two threads reading the
same shared file-loaded attribute that way still raced (TSan-confirmed at
`src/timesamples.cc` get_samples). Fix: a `mutable std::atomic<bool>
_samples_ready` guards the one-time build — lock-free acquire fast path once
built, a function-local static mutex for the cold initial build, release-store on
completion. Materialization stays lazy (no eager memory cost); the flag is reset
in `invalidate_reconstructed_samples_cache()` (and copy/move/clear handle it).
Reads are now genuinely pure once built. Verified with the standalone TSan
harness below (race before, clean after).

### ParserProfiler global singleton — **FIXED**
`LIGHTUSD_PROFILE_FUNCTION`/`_SCOPE` (`src/parser-timing.hh`) unconditionally called
`ParserProfiler::GetInstance().GetTimer(name)` → `timers_[name]` (mutating a shared
`std::map`) at the top of *every* parse, regardless of the default-off
`enable_profiling`. Two threads each calling `LoadUSDFromFile` raced on that map
(independent of pcp). Fix: (a) the scope macros now pass a null timer when
profiling is disabled (`ScopedTimer` no-ops on null), so the default build never
touches the singleton — zero overhead; (b) `timers_` is guarded by a new
`std::mutex ParserProfiler::mu_` in `GetTimer`/`GenerateReport`/`ClearAll` for the
enabled path (`std::map` keeps element pointers valid across inserts, so the
returned `ParserTimer*` stays valid after unlock). Per-timer counters remain
approximate under concurrent profiling; set config before spawning threads.
Covered by `stage_concurrent_parse_test`.

### CRC32 / base122 double-checked table init — **FIXED**
`usdz_crc32_table` (`src/lightusd.cc`), the identical table in
`src/writer/usdz-writer.cc`, and `base122_decode_map` (`src/base122.cc`) used
a hand-rolled `static bool …_ready` flag with no atomics → racy on concurrent USDZ
write/decode. Replaced each with a function-local `static const std::array<…>`
returned by reference (C++11 magic statics: thread-safe one-time init). The CRC
polynomial / initial values are byte-for-byte unchanged.

### TimeSamples lazy sort on read — **FIXED (eager-finalize at parse)**
`value::TimeSamples` const reads (`size()`, `get()`, …) call `update()`, which
sorts `mutable` parallel arrays in place and flips `mutable _dirty` — racy if a
shared time-sampled attribute is read from two threads. Fix: `update()` is now
called once (single-threaded) at the parse chokepoints — USDA in
`src/ascii-parser-props.cc` (after `ParseTimeSamples`/`…OfArray`), USDC in
`src/crate-reader-values.cc` (after `ReadTimeSamples`) — so every file-loaded
TimeSamples is sorted/clean before exposure and subsequent const reads are pure.
`update()` is public + idempotent; user-built TimeSamples (`add_sample`) must call
it before sharing for concurrent reads (documented on the method). Mirrors the
"compute eagerly so reads are pure" approach used for `Path`/`Xformable`. Covered
by `stage_concurrent_timesamples_read_test`.

### LayerRegistry resolver-cwp save/restore — **HARDENED**
`LayerRegistry::GetOrLoad` (`src/pcp/layer-registry.cc`) set→resolve→restored the
shared resolver's working path manually; an early return/throw would leak a stale
cwp to the next worker. Now a local RAII guard restores it on every exit path
(still inside the registry lock). Behavior otherwise unchanged.

---

## Remaining issues

None. All audited items are fixed or documented above. The full unit suite is
TSan-clean (see *Reproducing*).

---

## Confirmed safe (no action)

- `PrimSpec` / `PrimMeta` / `LayerMetas` / `Reference` / `Payload`: no `mutable`
  members → concurrent metadata reads are safe.
- `value::AssetPath::GetAssetPath()` returns a member ref (pure);
  `security_policy::ValidateAndNormalizeAssetPath()` and the namespace-mapping
  `Make*` / `Compose` helpers are pure (local copies, no static/global state);
  `Path::prim_part()` / `full_path_name()` / `is_valid()` are pure.
- No static mutable variables in `composition-graph.cc` / `pcp/*.cc`.
- `LayerRegistry` mutates only under its own mutex (which also serializes the
  shared `AssetResolutionResolver`'s working-path); each `CompositionContext` is
  per-entry/private; `BuildParallel` writes pre-sized `slots[]` at distinct
  indices; the cache maps (`index_cache`, `site_to_indices`, `index_to_sites`)
  are mutated only in the single-threaded merge after the join barrier.

Round-2 audit, confirmed safe with no change:

- **MCP server** (`src/tydra/mcp-server.cc`): CivetWeb runs 4 worker threads, but
  `Impl::process_request()` holds a single global `std::mutex mu_` over the
  *entire* handler, so all tool execution is serialized — `mcp_ctx_` (Stage,
  layers, js_engine) and the `g_js_stage` global are never touched concurrently.
  The module is `OFF` by default (`LIGHTUSD_WITH_MCP_SERVER`) and example-only.
  Note: that safety rests entirely on the coarse lock; removing/narrowing it would
  expose races on `mcp_ctx_` / `g_js_stage`.
- **`MapExpr::GetComposed()`** (`src/composition-graph.hh`): mutates a `mutable
  _composed_cache`, but the `MapExpr` pool is private to each worker's
  `PrimIndexBuilder`/`CompositionContext`, and the lazy path is currently dead
  (the only caller is `GetComposed` itself; `MapExpr::Apply` is unused). Not
  reachable concurrently.
- **`CrateReader`** mutable state (`_shared_times_cache`, decompression buffers,
  `_err`/`_warn`): layer parsing is parse-once-serialized under the
  `LayerRegistry` mutex and each `CrateReader` instance is used by a single
  thread, so its `const`-method mutations are never concurrent.
- **`Stage::_err` / `_warn` / `_prim_id_allocator`**: written only on parse/compose
  paths (non-const, or the const-but-parse-time `allocate_prim_id` reached only
  via `compute_absolute_prim_path_and_assign_prim_id`), never on the concurrent
  `GetPrimAtPath` read path — so they are not guarded by `_cache_mu`.
- **tydra** spawns no threads of its own; `attribute-eval.cc`'s clip cache is
  mutex-guarded and `render-data-material.cc`'s connection cache is
  `thread_local`.

Round-3 audit, confirmed safe / contract (no change):

- **Read/write contract for `Layer` / `Stage`**: the lock-free tree walk in
  `find_primspec_at` / `GetPrimAtPath` and the returned `const PrimSpec*`/`Prim*`
  are safe under the standard **concurrent-reads XOR exclusive-write** contract.
  Mutators (`add_primspec`, `add_root_prim`, compose/flatten) are single-threaded
  build APIs — never mutate a `Layer`/`Stage` while other threads read it. Cache
  inserts don't invalidate returned data pointers (they point into the stable
  `_prim_specs`/`_root_nodes` tree, not the cache map).
- **`prim-pprint-parallel.cc`** drain loop is correct: `producer_done` is stored
  only *after* every `Push()` returns, and `Push` returns only after the Vyukov
  `seq.store(release)` publish, so `done==true` ⟹ all items published; a worker
  that claimed a slot owns it. No lost-task race.
- **`value::any_value`** (no mutable members; `cast()` const-path is a pure
  reinterpret) and **`Animatable<T>`** scalar reads are pure. The only lazy state
  was `TimeSamples` (now eager-finalized at parse).
- **User-built `TimeSamples`** (via `add_sample`, not from a file) must be
  `update()`-finalized once, single-threaded, before sharing for concurrent reads
  (documented on `TimeSamples::update()`).

---

## Reproducing with ThreadSanitizer

> **IMPORTANT — the acutest unit-test binary is NOT a reliable TSan race
> detector.** A deliberately-injected data race inside a `unit-*.cc` test goes
> *unreported* by `./build-tsan/unit-test-lightusd` (both the in-process
> single-test path and the forked full-suite path), even though the binary is
> TSan-instrumented and prints the "Running under ThreadSanitizer" banner. The
> same race compiled as a small standalone IS caught. Root cause undetermined
> (some acutest × TSan × large-binary interaction). Treat a "0 races" from the
> unit-test binary as *weak* evidence only. TSan crashes/stack-smashes still
> surface (as `Unexpected exit code [66]` from the forked child), and the suite
> is still useful for **functional** concurrency checks.
>
> **Authoritative TSan check: a standalone that links the TSan-built static
> library** and exercises the shared object directly. This reliably reports
> races (verified: it flags the get_samples() race with the fix reverted, and is
> clean with the fix):
>
> ```bash
> cmake --build build-tsan --target lightusd_static -j16
> # write a small main() that builds the shared object and reads it from N threads
> g++ -std=c++17 -fsanitize=thread -O1 -g -Isrc -Isrc/external repro.cc \
>     build-tsan/liblightusd_static.a -lpthread -ldl -o repro
> setarch -R ./repro        # exit 66 + "WARNING: ThreadSanitizer" on a real race
> ```

```bash
# Build a TSan unit-test binary (threads on).
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DLIGHTUSD_BUILD_TESTS=ON \
  -DLIGHTUSD_ENABLE_THREAD=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -O1 -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan --target unit-test-lightusd -j16

# NOTE: modern Linux kernels make TSan abort at startup with
#   "FATAL: ThreadSanitizer: unexpected memory mapping"
# due to high ASLR entropy. Run under `setarch -R` (disable randomization),
# or lower vm.mmap_rnd_bits (needs root).
setarch -R ./build-tsan/unit-test-lightusd pcp_mt_shared_reference_test \
                                            pcp_singlethread_vs_multithread_identical_test \
                                            stage_concurrent_find_prim_test \
                                            stage_concurrent_parse_test \
                                            stage_concurrent_timesamples_read_test
# Full suite:
setarch -R ./build-tsan/unit-test-lightusd
```

The **entire** suite must report **0 data races** — including
`task_queue_multithreaded_test` and `stage_concurrent_find_prim_test`. Any race
whose stack mentions `src/pcp/`, `src/layer.cc`, `src/stage.cc`,
`src/composition-graph.cc`, `src/task-queue.hh`, or `src/tiny-hashmap.hh` is a
regression in this work.

After any change, also run the normal builds:
```bash
cd build && cmake --build . --target unit-test-lightusd -j16 && ./unit-test-lightusd   # threads off (canonical)
# threads on: configure a separate build dir with -DLIGHTUSD_ENABLE_THREAD=ON
cd web && cmake --build build -j16                                                     # wasm (single-threaded path)
```

Constraint for all fixes: keep non-threaded and wasm builds zero-overhead — gate
synchronization on `LIGHTUSD_ENABLE_THREAD` and never add a mutex member that
breaks `Layer` copy/move.

ODR gotcha (learned the hard way): `LIGHTUSD_ENABLE_THREAD` is **PRIVATE** to the
library target (`CMakeLists.txt`), so it is **not** defined when a consumer TU
(e.g. the unit tests) compiles a public header. Never `#if`-gate a *data member*
of a public-header class (e.g. `Stage`, which is held by value across the
library/consumer boundary) on it — that makes `sizeof` differ between TUs and
corrupts the stack. Gate only the *locking code* in `.cc` files; keep the mutex
member unconditional. (`LayerImpl::_cache_mu` is safe to gate only because
`LayerImpl` is pimpl'd — `sizeof(Layer)` is macro-independent.)

---

## Maintenance note

All audited items are resolved; the full unit suite is TSan-clean. Rules to keep
in mind when touching this code in the future:

- Gate all synchronization on `LIGHTUSD_ENABLE_THREAD`; keep non-threaded and
  wasm builds zero-overhead.
- Never add a mutex member that breaks `Layer` copy/move — use
  `shared_ptr<mutex>` (and reset it in `Layer`'s copy paths so copies don't share
  it).
- Don't add new `mutable` compute-and-cache state to `Path` / `Layer` / `PrimSpec`
  that a `const` accessor writes; either compute by value or guard it with the
  gated `LayerImpl::_cache_mu`.
- After any change, verify with ThreadSanitizer per the *Reproducing* section
  (build-tsan + `setarch -R`) and run the full unit suite threads-off and
  threads-on. Expect **0 races** across the whole suite.

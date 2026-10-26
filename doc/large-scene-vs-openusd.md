# LightUSD vs OpenUSD v26.11.1 on the benchmark scene scenes

Reference measurement of the same work in pxrUSD, for context on what the
`src/next` rounds (see [large-scene.md](large-scene.md) §10) bought, plus the
USDA text-syntax corner that the comparison uncovered.

Examples use `$WORK_ROOT`, `$BENCH_ROOT`, and `$ASSET_ROOT` for local checkouts
and scene data. Set them to the corresponding paths before running commands.

- Host: Ryzen 9 3950X (16C/32T, ~19x effective), 60 GiB RAM, Ubuntu 26.04,
  gcc 15.2. Both tools Release, outputs to `/dev/shm` (tmpfs).
- OpenUSD: **v26.11.1** (commit `96c198916`), built from `$WORK_ROOT/OpenUSD`.
- LightUSD: `lusdcat` from `build_ninja` (R22 + the round-23 parallel
  `BuildTables`), default flatten options
  `-f --compose-threads 0 --load-payloads --parse-threads 0 --write-threads 0
  --output-format usdc`.
- Raw measurement records and the scripts live in `$BENCH_ROOT`
  (`usd-26.11-vs-lightusd.md`, `usd_phase_bench.py`, `benchmark_*_pxr.usda`).

## 1. Building OpenUSD v26.11.1 (Python on, everything else off)

```sh
cd $WORK_ROOT/OpenUSD && git checkout v26.11.1
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=$WORK_ROOT/USD-26.11.1 \
  -DCMAKE_INSTALL_RPATH=$WORK_ROOT/python-3.13/lib \
  -DPython3_EXECUTABLE=$WORK_ROOT/python-3.13/bin/python3 \
  -DPXR_ENABLE_PYTHON_SUPPORT=ON \
  -DPXR_BUILD_TESTS=OFF -DPXR_BUILD_EXAMPLES=OFF -DPXR_BUILD_TUTORIALS=OFF \
  -DPXR_BUILD_IMAGING=OFF -DPXR_BUILD_USD_IMAGING=OFF -DPXR_BUILD_USDVIEW=OFF \
  -DPXR_BUILD_USD_TOOLS=ON \
  -DPXR_BUILD_ALEMBIC_PLUGIN=OFF -DPXR_BUILD_DRACO_PLUGIN=OFF \
  -DPXR_BUILD_EMBREE_PLUGIN=OFF -DPXR_BUILD_PRMAN_PLUGIN=OFF \
  -DPXR_BUILD_OPENCOLORIO_PLUGIN=OFF -DPXR_BUILD_OPENIMAGEIO_PLUGIN=OFF \
  -DPXR_ENABLE_GL_SUPPORT=OFF -DPXR_ENABLE_VULKAN_SUPPORT=OFF \
  -DPXR_ENABLE_METAL_SUPPORT=OFF -DPXR_ENABLE_OSL_SUPPORT=OFF \
  -DPXR_ENABLE_OPENVDB_SUPPORT=OFF -DPXR_ENABLE_MATERIALX_SUPPORT=OFF
cmake --build build-release -j32      # 3291 targets, 6 min 44 s (126 min CPU)
cmake --install build-release
```

Notes for this host:
- **Boost is not required** in v26.11.1: `find_package(Boost)` only runs with
  `PXR_ENABLE_OPENVDB_SUPPORT=ON`. Threads + oneTBB (system 2022.3) + Python3
  Development are the only dependencies.
- There are no Python development headers installed here
  (`/usr/include/python3.14` holds only PIL's headers) and no sudo, so CPython
  **3.13.7** was built from source into `$WORK_ROOT/python-3.13`
  (`--enable-shared --with-ensurepip`); USD's install RPATH points at its
  `lib/`.
- `PXR_BUILD_USD_VALIDATION=OFF` breaks the `usdfixbrokenpixarschemas` tool
  target, so validation stays ON (internal library, no external deps).
- Runtime: `LD_LIBRARY_PATH=$WORK_ROOT/python-3.13/lib:$WORK_ROOT/USD-26.11.1/lib`,
  `PYTHONPATH=$WORK_ROOT/USD-26.11.1/lib/python3.13/site-packages`.

## 2. Inputs

`benchmark_mine_full.usda` (one district) and `benchmark_fullLOD.usda` (all 45
districts) are small wrappers that sublayer `$ASSET_ROOT/benchmark/benchmark.usda`
and select the `districtLod = "full"` variant on each district prim. Both nest
their `over` prims on one line:

```usda
over "world" { over "mp_wz_reference_scene" { over "mp_wz_reference_scene_paths" {
```

pxrUSD's text parser rejects that (`Expected } at 'over "mp_wz_reference_scene_paths" {'`),
because a nested prim spec must be followed by a line break in the USDA grammar
— the wrappers were simply never pxr-parseable. They were re-emitted with one
prim body per line (`benchmark_{mine,fullLOD}_pxr.usda`, plus
`benchmark_full_n{1,5,15,30,45}_pxr.usda` for cumulative district subsets).
`lusdcat` on the reformatted wrappers reproduces the reference hashes exactly
(district `98619c40…`, fullLOD `a0175bc1…`), so the reformat is
semantics-preserving. See §7 for the full analysis of the syntax rule — it is
spec-conformant in OpenUSD, a permissive extension in LightUSD, and it also
exposed a genuine OpenUSD diagnostic bug that has been written up for upstream
(the report drafts are kept outside this repository).

## 3. Method

`$BENCH_ROOT/usd_phase_bench.py` splits the pxr pipeline into the
phases that LightUSD's `lusdcat` reports:

| phase | pxr call | LightUSD phase |
|---|---|---|
| load | `Usd.Stage.Open` (lazy: layer stack, prim index, payloads) | part of `load+compose` |
| traverse | `stage.Traverse()` | — |
| compose | `stage.Flatten()` → `Sdf.Layer` | rest of `load+compose` |
| write | `Sdf.Layer.Export(out.usdc)` | `write` |

Cross-checked with the CLI: `usdcat -f -o out.usd --usdFormat usdc` takes
126.7 s / 11.3 GB on the district vs 126.1 s / 11.35 GB from the harness
(the CLI adds a source-file comment to `documentation`, hence not byte-equal).

## 4. District (`benchmark_mine_full`, 724 700 stage prims, 9.5 M paths)

| phase | OpenUSD v26.11.1 | LightUSD | ratio |
|---|---:|---:|---:|
| load + compose (`Open` + `Flatten`) | 80.33 s (2.65 + 77.68) | **3.92 s** | 20.5x |
| write (`Export` → USDC) | 45.20 s | **2.33 s** | 19.4x |
| total | 126.10 s | **6.25 s** | **20.2x** |
| peak RSS | 11.35 GB | 4.79 GB | 2.37x |
| output | 500.9 MB | 580.3 MB | |

Repeatability: pxr 126.1 / 126.2 s (harness), 126.7 s (CLI); LightUSD 6.20 /
6.25 s.

## 5. Cumulative districts (`benchmark_full_nN`)

`paths` = prims + properties counted on the written USDC (pxr reports ~13
properties per prim on this scene). Both tools compose the same scene at 1–1.5%
different prim counts.

| n | stage prims | paths | pxr load+compose | pxr write | pxr total | pxr RSS | LightUSD load+compose | LightUSD write | LightUSD total | LightUSD RSS | total ratio |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 724 700 | 9.5 M | 80.3 s | 45.2 s | 126.1 s | 11.4 GB | 3.9 s | 2.3 s | 6.3 s | 4.8 GB | 20.2x |
| 5 | 1 632 126 | 23.8 M | 187.3 s | 98.3 s | 286.8 s | 20.5 GB | 7.3 s | 4.5 s | 11.8 s | 10.5 GB | 24.3x |
| 15 | 2 015 184 | 28.8 M | 227.1 s | 111.6 s | 340.3 s | 23.2 GB | 9.0 s | 5.4 s | 14.4 s | 12.6 GB | 23.6x |

Composed prim counts on the flattened output: pxr 724 700 / 1 632 126 /
2 015 184 vs LightUSD 735 484 / 1 646 857 / 2 032 455 (1.0–1.5% more; the
difference is in how instance prototypes and some `over` prims are
flattened, not in scale).

## 6. full45 (`benchmark_fullLOD`, all 45 districts)

10 187 143 stage prims, ~132 M written paths (LightUSD's own path records for
this scene are 61.8 M; that counter does not include every property).

| | OpenUSD v26.11.1 | LightUSD |
|---|---:|---:|
| load (`Open`) | 34.17 s | 38.25 s (load + compose) |
| traverse of all 10.2 M prims | 8.45 s | — |
| compose (`Flatten`) | **not runnable on 60 GiB** | included above |
| write | — | 24.90 s |
| total | — | 63.15 s |
| peak RSS | 24.2 GB (load only) | 29.9 GB (whole pipeline) |
| output | — | 3.01 GB |

`Flatten` memory grows ~0.6 GB per 1 M written paths beyond the stage itself
(11.4 GB @ 9.5 M → 20.5 GB @ 23.8 M → 23.2 GB @ 28.8 M), i.e. ~24 + 0.6 x 132
≈ 100+ GB for full45, which does not fit this host, so its compose and write
phases could not be measured (runs were capped with `ulimit -v 44000000`;
load-only succeeded). Note that pxr's *load alone* (42.6 s including the
traverse, 24.2 GB) is already 0.67x LightUSD's entire compose + write.

## 7. Reading the numbers

- pxr's `Open` is lazy: almost all composition cost sits in `Flatten`.
  LightUSD's single `load+compose` phase does that work 20–24x faster, and the
  gap grows slightly with scene size.
- pxr's flattened-layer `Export` is the other half of the cost (~11 MB/s),
  19x slower than LightUSD's streaming writer; per output byte it is ~23x
  because LightUSD's crate is 15% larger for the same scene.
- Peak memory is 1.8–2.4x higher for pxr, and it is what makes the full
  45-district scene unmeasurable there, while LightUSD composes + writes it in
  63 s / 30 GB.
- These are end-to-end wall-clock numbers for the same input file and the same
  output format (USDC text-free crate), not microbenchmarks.

## 8. USDA text syntax: newline after a prim spec

The comparison needed pxr-parseable inputs and uncovered a rule that is easy to
trip over. It is documented here because it affects how USDA files (hand-written
or generated) may be formatted. Reproduction scenes and the upstream issue
draft were prepared for the report and are kept outside this repository.

The AOUSD Core Spec 1.0.1 (`aousd/core/1.0.1/core_spec.md`, verbatim upstream
copy, commit `2f9e746c`) defines:

```peg
StatementSeparator <- Crlf / Semicolon                                  # l.8826
PrimItem <- ChildOrPropertyOrderStatement (SinglelinePadding)* StatementSeparator /
            PropertySpec (SinglelinePadding)* StatementSeparator /
            PrimSpec (SinglelinePadding)* Crlf /                         # l.9764
            VariantSetStatement (SinglelinePadding)* Crlf
PrimContents <- (MultilinePadding)* (PrimItem (MultlinePadding)*)*       # l.9769
LayerItem <- 'reorder' (InlinePadding)+ 'rootPrims' Assignment NameList / PrimSpec   # l.10031
LayerSpec <- LayerHeader (MultilinePadding)* (LayerMetadata (MultilinePadding)*)? ↵
             (LayerItem (MultilinePadding)*)*                            # l.10035
```

So, per the spec:
- a **nested** prim spec (or `variantSet`) must be followed by a line break
  (`Crlf`); a semicolon does **not** substitute for one;
- a **property** spec may be followed by a line break *or* `;`;
- at the **layer top level**, prim specs are separated by `MultilinePadding`
  (which includes a plain space) — no line break required.

The rule is unchanged in **Core 1.1.0** (`core/v1.1.0`, commit `83e25ace`,
2026-08-13; vendored verbatim at [`aousd/core/1.1.0/`](../aousd/core/1.1.0/)):
`PrimItem`, `LayerItem`, `LayerSpec` and `StatementSeparator` are textually
identical to 1.0.1 — the only change in that area is a `MultlinePadding` typo
fix in `PrimContents` — and the prose still never says that line breaks are
significant. The spec's own §"Suggested Simplifications To the Grammar" (1.1.0
l. 11522) already lists "Make statement separators consistent", so this is a
known wart rather than a settled design, and a spec-side report has been
drafted for upstream.

OpenUSD implements exactly that for prim contents
(`pxr/usd/sdf/textFileFormatParser.h`): `PrimItem` ends its prim alternative
with `Eol`, `StatementSeparator` is `sor<Eol, ';'>`, `SinglelinePadding` is
`sor<Space, Comment>`, and `PrimContents` may match zero items. But at the layer
level `LayerSpec` requires `Eolf` after every root `PrimSpec`, which is
*stricter* than the spec's `LayerItem`.

Behavior matrix (20 cases, `usdcat -l` on OpenUSD v26.11.1 vs
`lusdcat --rewrite-layer`):

| case | AOUSD 1.0.1 | OpenUSD 26.11.1 | LightUSD |
|---|---|---|---|
| `over "a" { over "b" {} }` (no newline after child) | invalid | **rejected**, `Expected } at 'over "b" {} }'` | accepted |
| `over "a" { over "b" {}\n}` | valid | OK | OK |
| `def "a" {} def "b" {}` (two roots, one line) | **valid** | **rejected**, `parse error matching …pegtl::internal::eof` | accepted |
| `def "a" { custom string x = "y" }` | invalid | rejected, `Expected }` | accepted |
| `def "a" { custom string x = "y"; }` | valid | OK | OK |
| `over "a" { over "b" {};\n}` | invalid | rejected | accepted |
| file without a trailing newline | valid | OK | OK |
| `over "a" { over "b" {} # c\n}` | valid | OK | OK |

Takeaways for LightUSD:
- its parser is a **superset** of the spec/pxr grammar here (it accepts a
  missing line break, a property directly before `}`, and a `;` after a prim);
  that is convenient for hand-written files but should be documented, and the
  *writer* must stay strictly conformant;
- the writer is conformant: `lusdcat --rewrite-layer` on each same-line case
  emits one prim body per line, and pxr `usdcat -l` accepts all such outputs
  (checked for A/C/D/H/L);
- OpenUSD's behavior for the nested case is spec-conformant; what is *not* is
  (a) the diagnostic (it points at the child prim and says `Expected }`) and
  (b) the layer-level `Eolf` requirement, which rejects spec-valid one-line root
  prims. Both were written up for upstream, with a two-line parser patch that
  was built and regression-tested against 3041 `.usda` fixtures (identical
  accept/reject, but the error then names the missing line break).
- Unrelated detail visible in the rewritten snippets: LightUSD's USDA writer
  omits the deprecated `custom` qualifier unless `--openusd-compat` is passed
  (the flag itself is preserved in USDC — `Usd.Attribute.IsCustom()` is true).
  That is deliberate (see `src/next/writer/usda-writer-print.cc`,
  `opts.emit_custom`), not a round-trip loss.

## 9. Reproducing

```sh
# pxr
cd $WORK_ROOT && export LD_LIBRARY_PATH=$PWD/python-3.13/lib:$PWD/USD-26.11.1/lib \
  PYTHONPATH=$PWD/USD-26.11.1/lib/python3.13/site-packages
python3.13/bin/python3 bench/usd_phase_bench.py bench/benchmark_mine_full_pxr.usda /dev/shm/usd.usdc
USD-26.11.1/bin/usdcat -f -o /dev/shm/usd.usd --usdFormat usdc bench/benchmark_mine_full_pxr.usda

# LightUSD
cd $WORK_ROOT/large-scene
./build_ninja/lusdcat -f --compose-threads 0 --load-payloads --parse-threads 0 \
  --write-threads 0 --output-format usdc --max-memory-mib 65536 \
  --max-crate-elements 300000000 -o /dev/shm/lu.usdc \
  $BENCH_ROOT/benchmark_mine_full_pxr.usda
```

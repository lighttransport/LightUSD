# verif/lean — Lean 4 formalization of the AOUSD USD Core Specification v1.1.0

A machine-checked model of the normative parts of
[`aousd/core/1.1.0/core_spec.md`](../../aousd/core/1.1.0/core_spec.md) and its
reflection into the LightUSD `src/next` implementation and validator.

## Build

```sh
export PATH=$HOME/.elan/bin:$PATH       # Lean 4.35.0 via elan (see lean-toolchain)
cd verif/lean
lake exe cache get                      # once: Mathlib v4.35.0 prebuilt oleans (network)
lake build                              # checks every definition and proof
lake exe usdspec-test                   # runtime sanity of the executable definitions
lake exe usdspec-gen --out ../../tests/next/generated/lean          # regenerate C++ vectors
lake exe usdspec-gen --out ../../tests/next/generated/lean --check  # drift check (CI)
lake exe usdspec-check-corpus <corpus>/data_types/tests/combine_chain
```

Unicode table and conformance gates use the official Unicode 15.1.0 UCD,
pinned by URL and SHA256 in `data/unicode-15.1.0.json`. From the repository root:

```sh
python3 scripts/fetch-lean-unicode-data.py
python3 scripts/test-lean-unicode-data.py --conformance
```

The download cache is ignored at `build_ninja/unicode-15.1.0`; cached files are
verified on every run. The XID generator reads `DerivedCoreProperties.txt`
independently and also checks the native C tables. Normalization tables read
`UnicodeData.txt` and `DerivedNormalizationProps.txt`; the host Python Unicode
version is irrelevant. Generators support `--ucd-dir` and `--check`. The actual
Lean NFC/NFD code passes the official 19,074-row normalization corpus and its
Part1 complement of 1,095,035 scalars (2,380,810 checks) plus a 3.8M-string
idempotence sweep over pairs/triples of composition components and marks. Four negative controls
reject corrupted reference bytes, native-table drift, generated-table drift and
missing data. CTest (`next_lean_unicode_conformance`) and CI require these gates.
These exhaustive reference checks are validation evidence; a general symbolic
NFC idempotence proof is still open. USD paths remain unnormalized.

The trust audit is `lake env lean Audit/Axioms.lean`. It inspects transitive
dependencies of every compiled project theorem, including private helpers, and
permits only `propext`, `Classical.choice`, and `Quot.sound`. The full audit passes
for 3,228 theorems (499 private) across 43 modules. The crate proofs use algebraic
integer reasoning and per-bit equality in `UsdSpec/Core/BvKernel.lean`; their
statements retain universally quantified inputs. `scripts/test-lean-axioms.py`
checks that the audit accepts kernel proofs and rejects explicit axioms, `sorry`,
and native decision tactics, including through imported module-style helpers.
CTest and the Lean CI workflow run both checks. The broader model/hypothesis
audit remains separate from this trust check.

The prim-index model now merges reference/payload/inherit/specialize list edits
and supports global payload LoadAll/LoadNone. A new Lean-generated JSON matrix
contains 256 layered cases and 512 queried prims with complete property stacks.
The production CLI and pinned stock OpenUSD match every expected stack. Run
`python3 tests/next/test-prim-index-vectors.py --lusdcat build_ninja/next_debug/lusdcat`
from the repository root; stock flags match `test-pcp-json.py`. CTest and CI run
the consumer, and the generator drift gate covers the JSON. See [the model audit](../../doc/validation/lean-prim-index-audit.md)
for remaining implied-class behavior, inherit diamonds whose namespace mappings
differ, relocates and variant limitations.

Sublayer traversal now uses a finite branch-local DFS instead of a fuel cutoff.
Its decreasing measure is the available-layer set cardinality. Cyclic/missing
edges are diagnosed and ignored, with proofs of branch exclusion and known-layer
membership. Legal shared diamond layers remain repeated. Another generated JSON
matrix covers 94 graph/error cases, including a 25-layer chain, against native
and pinned stock PCP. `next_lean_layerstack_vectors` runs the native consumer;
CI also requires stock. Prim-arc expansion now terminates by a finite active-site
set and reports skipped cycles through `composePrimIndexChecked`. A checked
self-reference example validates that path. Specializes ordering drains nested
cohorts before weaker siblings (`A, B, D, C`); native PCP and pinned stock agree.
Origin-aware class coalescing and path-renamed cohorts remain open.

The spline evaluator's rational Keep Ratio calculation is certified at arbitrary
fuel: its scale is positive and at most one and the corrected time cubic is
non-regressive. `SplineBounds.lean` proves a unique real time root and midpoint
value error at most `K/2^(n+1)`, with an explicit control-point constant `K`.
The proofs include fuel zero and isolated zero derivatives, and require no
rational-root assumption. See [the spline bounds audit](../../doc/validation/lean-spline-bounds-audit.md)
for assumptions, the exact code linkage and the native floating-point boundary.

The element-name token-key model now has a general tokenizer left-inverse proof
(`detokenize_tokenize`), tokenizer injectivity, joint primary/secondary key
injectivity, and comparator equality exactly when strings are equal. Together
with the strict transitivity and swap laws, this gives trichotomy and non-strict
antisymmetry without bounded-name assumptions. This proves the mathematical
ordering properties of that model; it does not make it identical to OpenUSD's
byte comparator. The literal/compatibility split is recorded per registry row in
`data/model-classification.json` (10 `dual` rows with distinct literal and
OpenUSD-compatible declarations, 7 `compat-observation` rows naming the
implemented side, 53 spec defects, 3 notes) and checked by
`scripts/check-lean-model-split.py`; citations are checked by
`scripts/check-lean-spec-citations.py`.

The C++ side consumes the committed vectors without needing Lean:
`ctest --test-dir build_ninja/next_debug -L lean` (the `next_lean_*` tests are
registered only when `lake` is found; `next_test_lean_vectors*` always run).

## Layout

| Path | Spec | Content |
|---|---|---|
| `UsdSpec/ListOp.lean` | 6.6.3 L1271-1708 | list ops: `iterate`, `⊔`, `reduce`, `chain`, `≅`; bridge theorems `iterate_combine`, `apply_combine`; congruence characterization; spec examples by `decide` |
| `UsdSpec/ListOp/Apply.lean` | — | `ApplyStringListOp` / `ApplyInt64Op` models proven equal to the spec `apply` on duplicate-free inputs |
| `UsdSpec/ListOp/Legacy.lean` | L1699-1708 | deprecated `add` / `reorder` |
| `UsdSpec/Dict.lean` | 6.6.2 L1241-1269 | dictionary union |
| `UsdSpec/LayerOffset.lean` | L4271-4298, L5428-5510 | offsets, TCPS |
| `UsdSpec/Path/*`, `UsdSpec/Unicode/*` | ch 8, L1805-1837 | path elements, element ordering, path PEG (`parse_print`), XID tables, NFC/NFD |
| `UsdSpec/Schema/*` | L8240-8950 | schema family/version, prim definition |
| `UsdSpec/Composition/*` | ch 10 | arc lists, LIVERPS strength order (examples 1-6), namespace maps, relocates, toy prim index, instancing |
| `UsdSpec/Population/Children.lean` | L5937-6004 | prim/property child ordering |
| `UsdSpec/ValueResolution/*` | ch 12 | metadata, attribute source priority, time samples, splines (Keep Ratio, loops, AutoEase), value clips |
| `UsdSpec/Crate/*` | L11532-12910 | header/TOC, ValueRep, integer coding, LZ4 block + framing, PATHS, list-op encoding |
| `UsdSpec/Usda/*` | L9569-11375 | subset parser/printer, generic PEG engine, full normative grammar (generated by `scripts/extract-usda-peg.py`) + repairs |
| `Gen/` | — | `usdspec-gen`: X-macro `.inc` vector tables for `tests/next/test_lean_vectors.cc` |
| `Check/` | — | `usdspec-check-corpus`: supplemental-corpus replay |
| `ISSUES.md` | — | registry of `SPEC-ISSUE: LEAN-NNN` tags → `issues/aousd/NN` |
| `REFLECTION.md` | — | Lean theorem ↔ C++ function ↔ validator rule ↔ test |

## Conventions

* Every normative definition is a computable `def` (`deriving DecidableEq, Repr`)
  so `#eval`, `decide` and the generator work; proofs are separate theorems.
* First docstring line cites the spec: `SPEC: core_spec.md L1421-1483 (6.6.3 Combining)`.
  A theorem that formalizes a literal claim adds `CLAIM: <quote>`.
* Where the formalization had to deviate from or disambiguate the text, the
  docstring carries `SPEC-ISSUE: LEAN-NNN — <one line>` and `ISSUES.md` has the
  row (`scripts/check-lean-issues.sh` enforces this).
* Element sequences are plain `List α`; uniqueness (spec L1308-1337) is a
  `List.Nodup` hypothesis where needed, never a subtype.
* Numbers in proof modules are exact (`ℚ`, `Int32`, …). `Float` appears only in
  generators and only for dyadic inputs so C++ `double` comparison is exact.
* Floating-point rounding behaviour of the C++ implementation is out of scope:
  parity is checked numerically by the vectors, not proven.

## Independent follow-up

The follow-up ledger is [lean-followup-status.md](../../doc/validation/lean-followup-status.md).
General proofs now characterize `mostCommon` and establish the greedy LZ4
compressor's end-of-block rules. PATHS trees round-trip for arbitrary signed
tokens and sparse path indices, with encoded row count as sufficient fuel and
a tight bound for chains of every length. The forest lemma permits arbitrary
prefix/suffix rows and accumulated output and covers all four jump forms.
Sparse indices are preserved in the decoded event list; this does not validate
binary table bounds, index uniqueness, or the separately encoded total count.
The USDA quoted-string codec also has a general inverse and an explicit fuel
bound, including arbitrary Unicode, controls and NUL. Its forty emitted vectors
are checked against native transports and stock OpenUSD; unrepresentable crate
NULs fail explicitly in LightUSD. The complete supported-AST round-trip theorem is in LayerProofs; the full
source-escape grammar remains outside this operational subset.
`Usda/GrammarProofs.lean` supplies boundary-sensitive inverses for the subset's
identifier, numeric-lexeme, asset and path-reference scanners. Its general list
composition theorem covers arbitrary list lengths, indentation, trailing input
and explicit item-domain assumptions; the path-reference list instance proves
the actual bracket printer used by inherits/specializes. Scanner character
conditions do not assert full PEG number validity, Unicode XID or semantic asset
and object-path validity. Recursive AST composition is proved in PrimProofs and LayerProofs.
`Usda/OffsetProofs.lean` proves offset, reference and sublayer inverses for every
supported optional-field combination and arbitrary reference/sublayer list
lengths. It establishes the local fuel and whitespace properties explicitly.
Scalar item inverses include the canonical comma boundary and preserve the
arbitrary suffix following that delimiter.
Its well-formedness predicates express lexical admissibility, including a
nonempty reference and delimiter safety; they do not replace semantic validation.
`Usda/SampleProofs.lean` proves exact sample-map recovery for arbitrary list
lengths and advances the actual attribute/body parser for both default and
time-sampled statements. Attribute-only bodies have a sufficient item-count
fuel bound. These are lexical AST results; timestamp validity and interpolation remain
separate semantic obligations.
`Usda/RecursiveMetadataProofs.lean` proves exact recursive dictionary recovery
with no nesting or entry-count bound. Its conservative parser cost is dominated
by printed length. Actual metadata statement and block inverses include all
four supported constructors, mixed arc qualifiers, nested customData, indentation
and arbitrary trailing input. Dictionary keys are arbitrary Strings; delimiter
and offset conditions remain lexical printer admissibility hypotheses; these results do not certify composition
validity or full normative PEG coverage.
`Usda/PrimProofs.lean` proves actual prim/body recovery by simultaneous fuel
induction, for arbitrary nesting, mixtures of attributes/child prims and metadata.
Printed length dominates its recursive cost. `Usda/LayerProofs.lean` assembles
actual prim lists, optional sublayers, header and EOF into `parseLayer_print`
under Layer.WellFormed. The printer quotes arbitrary dictionary keys;
EntriesWellFormed_all proves every dictionary tree admissible, and the old
quoted-key counterexample now recovers exactly. Remaining lexical hypotheses
match the supported identifier/number scanners and single-delimiter asset/path
syntax; no nesting, list-count, string-length or numeric-magnitude cap is added.
The [constructor audit](../../doc/validation/lean-usda-roundtrip-audit.md)
distinguishes this complete supported-AST inverse from full PEG coverage and
semantic validity. The seventeenth generated table adds 135 actual Lean-printed
key cases, checked through native transports and pinned stock OpenUSD. Native
writing quotes namespace separators too; NUL keys fail explicit crate writing
rather than truncating them.
Strength ordering uses unbounded integer depth
keys. All previously native-decided examples now use kernel-checked proofs;
the checked theorem dependencies contain only standard Lean axioms. These
changes do not establish the remaining parser, binary-table, NFC, or prim-index
proof obligations listed in the ledger.

For exact validator references, run:

```sh
python3 scripts/check-lean-reflection.py --check --lake \
  --lusdchecker build_ninja/checker/lusdchecker --require-lusdchecker
python3 scripts/test-check-lean-reflection.py
```

Run these commands from the repository root after building Lean and the checker.
The dedicated Lean CI workflow runs proofs, runtime tests, vector drift, and
required checker reflection.

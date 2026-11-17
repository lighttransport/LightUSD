# tinysubdiv (tsd)

Clean-room, dependency-free subdivision surface library for LightUSD.
C++17 in a C-ish style: POD input views, no exceptions, no RTTI.

## Features

- Uniform refinement: **Catmull-Clark**, **Loop** (all-triangle input),
  **Bilinear** — numerically verified against OpenSubdiv (`Far::
  TopologyRefiner`/`PrimvarRefiner`) across the full USD feature matrix.
- USD/AOUSD subdivision semantics:
  - `interpolateBoundary`: `edgeAndCorner` (default), `edgeOnly`, `none`
  - `faceVaryingLinearInterpolation`: all 6 modes (`cornersPlus1` default)
  - semi-sharp creases (`creaseIndices`/`creaseLengths`/`creaseSharpnesses`,
    per-crease or per-edge sharpness), `uniform` and `chaikin` creasing
  - corners (`cornerIndices`/`cornerSharpnesses`)
  - face holes (`holeIndices`), propagated and filtered
  - `triangleSubdivisionRule`: `catmullClark` (default), `smooth`
- Primvar refinement: `vertex` (smooth), `varying` (linear), `faceVarying`
  (seam-split smooth or per-corner linear); `uniform` replicates via
  `RefinedMesh::face_source` on the caller side.
- Closed-form limit positions (`SnapToLimit`) and limit normals
  (`ComputeLimitNormals`) for Catmull-Clark and Loop.
- **Streaming refinement** (`RefineStream`): refines to level N without ever
  materializing the full output -- the final (largest) level is emitted to a
  sink in bounded batches, so peak working memory is the level-(N-1) state plus
  one batch (geometry + vertex/varying primvars + faceVarying, both linear and
  smooth seam-split + seamless limit normals). With `StreamOptions::block_faces`
  it also bounds the
  *working set* (block + halo refinement) for meshes too large to refine whole.
  This is what lets deep refinement fit in wasm32's 2 GB; exposed to JS via the
  `SubdivStreamer` binding and demoed in `web/js/subdiv.js`.
- Hardened: full input validation, 64-bit overflow-checked sizing,
  per-level vertex/face/corner caps, fuzz-tested (`tests/fuzzer/fuzz_tsd.cc`).
  Non-manifold input (including inconsistent face winding) is **rejected** —
  an intentional divergence from OpenSubdiv, which tolerates it.
- Optional threading through an injected `parallel_for` callback with
  bit-identical results.
- **Adaptive bilinear refinement** (`RefineAdaptive`): quadtree-style,
  predicate-driven. Each round a callback sees the faces created in the
  previous round and marks which to split; marked faces are replaced by their
  bilinear children (T-junctions between levels are intended). Leaves record
  base face, depth and parent (to carry per-face data down). Used to partition
  2D parameter domains, e.g. branch-and-bound certification in
  materialx-conformance `tools/gpu/certify_albedo_gpu.hip`.

## Files

| File | Contents |
|---|---|
| `tinysubdiv.hh` | public API (the only header to include) |
| `tsd-internal.hh` | internal shared declarations |
| `tsd-validate.cc` | input validation, USD crease canonicalization |
| `tsd-topology.cc` | edge interning, CSR adjacency, child topology |
| `tsd-catmark.cc` | Catmull-Clark kernel + sharpness decay rules |
| `tsd-loop.cc` | Loop kernel |
| `tsd-bilinear.cc` | bilinear kernel (also the "varying" linear path) |
| `tsd-fvar.cc` | faceVarying: seam-split smooth + linear paths |
| `tsd-kernel.hh` | per-element value kernels shared by bulk + streaming |
| `tsd-refine.cc` | per-level driver (`Refine`) |
| `tsd-stream.cc` | streaming driver (`RefineStream`): bounded-memory emission |
| `tsd-limit.cc` | limit positions/normals |
| `tsd-adaptive.cc` | adaptive (selective) bilinear refinement (`RefineAdaptive`) |
| `tsd-lightusd.{hh,cc}` | `GeomMesh` adapter (the only lightusd-typed file); refines UV/display/tangent primvars, skin weights and blendshape offsets in lockstep with geometry |

## Testing

- `feat-subdiv` (ctest, always built): analytic rule checks, invariants,
  hardening and determinism tests. No external dependencies.
- `feat-subdiv-verify` (opt-in): exhaustive comparison against an
  OpenSubdiv **source checkout** (no pre-build needed):

```sh
cmake -B build -DLIGHTUSD_BUILD_TESTS=ON \
      -DLIGHTUSD_TSD_VERIFY_WITH_OSD=ON \
      -DOpenSubdiv_ROOT=/path/to/OpenSubdiv
cmake --build build -j --target feat-subdiv-verify
ctest --test-dir build -L osd-verify
```

The comparator is ordering-independent (vertices matched by position on
jittered corpora, faces compared as rotation-canonicalized sets) and checks
positions, topology, fvar values, hole filtering, limit positions and limit
normals. Limit normals at interior creases and infinitely sharp corners are
convention-dependent in OpenSubdiv itself and are checked structurally
(unit length, perpendicularity to the crease) instead of bitwise.

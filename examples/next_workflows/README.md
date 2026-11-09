# Small next API workflows

These examples use `lightusd::next` and `lightusd::tydra::next`. Inputs are
caller-supplied; generated files go to explicitly named output paths.

Build from the repository root:

```sh
cmake -S . -B build_ninja -G Ninja -DLIGHTUSD_BUILD_EXAMPLES=ON -DLIGHTUSD_BUILD_TOOLS=ON -DLIGHTUSD_BUILD_TESTS=ON
cmake --build build_ninja -j8
ctest --test-dir build_ninja -R '^(next_workflow_examples|workflow_tools|next_gltf_export)$' --output-on-failure
```

The four C++ examples also build in the standalone `src/next` project with
`LIGHTUSD_NEXT_BUILD_EXAMPLES=ON` (default for standalone builds).

| Executable | What it demonstrates |
|---|---|
| `next_quickstart INPUT OUTPUT.usda` (or `.usdc`) | Bounded composition, traversal, typed evaluation, writing and reopening |
| `interactive_session ROOT` | Retained snapshots, `/World`'s `display=high` selection, deferred payload load/unload, reload, cancellation, transactional `SceneUpdateSink` |
| `animation_sampling INPUT /Prim.attribute` | DefaultTime versus numeric time; held/linear evaluation, spline evaluation, lazy clip loader/cache, sampled SkelAnimation extraction |
| `portable_asset_package NEW_DIRECTORY` | Memory assets/custom resolution, dependency inventory, validation, USDC writing, USDZ packaging, relocation and reopening |

`interactive_session` expects a `/World` prim with a `display` variant set and a
`high` option. Its logging sink shows the update protocol; it is not a GPU
renderer. `RenderSession` currently reconverts render-affecting snapshots before
emitting changed resources. It preserves the old published render snapshot if a
sink rejects an update.

`animation_sampling` samples default, 0, 0.5 and 1. Supply an attribute with a
default if you want all four queries to succeed. Clip stages are loaded on demand.
The integration test generates ordinary samples, a linear spline, and a joint
animation; externally authored clip assets can use the same example.

## Synthetic capture

`synthetic-capture.py` generates a tiny mesh, two cameras, RenderSettings,
RenderProducts and raw RenderVars, invokes `lusdrender` once, checks the data
images numerically, and writes a camera/output manifest:

```sh
python3 examples/next_workflows/synthetic-capture.py \
  --lusdrender /path/to/built/lusdrender --output-dir /tmp/new-capture
```

The output directory must not exist. Each camera receives color PNG, raw
float32 depth/world-normal/prim-ID PFM, and an ID-to-source-prim JSON table.
The manifest states the sampling and coordinate conventions. No external asset,
network service, display or GPU is required.

See [workflow tool contracts](../../doc/workflow-tools.md) for data semantics,
limits and currently unsupported cases.

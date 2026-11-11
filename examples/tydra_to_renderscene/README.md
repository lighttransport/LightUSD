# RenderScene diagnostics

Build from the repository with `LIGHTUSD_BUILD_EXAMPLES=ON`, or configure this
directory as a standalone CMake project. The executable uses core loading,
composition and Tydra; legacy is not required.

```sh
./build_ninja/tydra_to_renderscene scene.usdz --json
./build_ninja/tydra_to_renderscene scene.usda --nodump --memstat --profile
./build_ninja/tydra_to_renderscene scene.usda --dumpobj --dumpusd
./build_ninja/tydra_to_renderscene animated.usda --nodump --dump-timesamples
```

`--help` lists the controls. `--core` is a compatibility alias for the core
default. JSON diagnostics contain source metadata, configuration, resource
counts, hierarchy, geometry previews, shader parameters and animation channels.
YAML presents the same records in block form. Large arrays are previewed;
`--dump-timesamples` prints complete animation sample values.

`--texload` decodes disk and archive images with bounded allocations, retaining
integer/HDR component formats. `--noar` permits the explicitly supplied root and
disables dependent file access and image decoding. `--lowmem` releases the source
Stage after conversion. `--mmap-lowmem` opts into trusted local mmap loading.
`--snorm8` retains signed-byte normal/tangent payloads; debug exporters reconstruct
the quantized float values when needed.

OBJ files contain mesh-local coordinates. USDA exports mesh instances with world
transforms, materials, subsets, skeleton bindings and blend shapes. USDA export
of animation clips remains unsupported and returns an error. Non-mesh render
resources omitted by that diagnostic exporter produce warnings.

The example builds unified vertex indices by default, preserving attribute seams,
skin weights, morph targets and material subsets. `--fast-index-build` expands
corners without deduplication; `--noidxbuild` retains separate attribute streams.
Vertices from different source points remain distinct. The core retained
converter continues to expose separate streams for consumers that need them.
`core_mesh_index` covers both modes, polygon topology, tangent winding, indexed
primvars, memory limits and invalid inputs.

Regression: `core_render_scene_example` and `core_render_scene_c_api_smoke`.

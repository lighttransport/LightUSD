# USD to GLB

This maintained example converts a composed next Stage through Tydra-next into
a self-contained glTF 2.0 binary:

```sh
./build_ninja/usd_to_gltf input.usdz output.glb --report losses.json
./build_ninja/usd_to_gltf input.usda output.glb --strict
```

Built with `LIGHTUSD_BUILD_EXAMPLES=ON`. The library entry is
`tydra/next/gltf-export.hh`: `ExportGLB(RenderScene, GltfExportOptions)`.
The command writes GLB only. `--strict` refuses output if conversion reports a
loss; otherwise losses appear on stderr, in the optional JSON report, and in
`extras.lightusdConversionLosses` (exporter losses) inside the GLB.

Supported: static triangulated mesh geometry, hierarchy/transforms, normals,
primary UVs, material subsets, per-mesh double-sided state, PreviewSurface
factors, PNG/JPEG images, and compatible base-color, normal, emissive, occlusion,
and already-packed metallic/roughness textures. The root converts stage units
to meters and Z-up to Y-up.

Animation, skinning, morph targets, point instancers, points/curves, lights,
cameras, displacement/volumes, texture transforms and separate metallic/roughness
channel packing are reported as losses. Additional UV sets and nonstandard
texture colorspaces also need conversion work; strict mode catches these cases.
Non-PNG/JPEG images require transcoding and are omitted with a loss. Invalid
geometry, non-finite data, cyclic hierarchies and output-budget overflow fail.
The default output budget is 1 GiB, including embedded images and JSON.

This is a static export path, not a USD roundtrip format. The integration tests
inspect geometry, material assignments and embedded image payloads; the direct
API test exercises malformed inputs and budget failures.

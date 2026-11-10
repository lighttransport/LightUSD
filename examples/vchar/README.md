# vchar

`vchar` is LightUSD's native virtual-human rendering profile. It is emitted
beside `lusdview` when `LIGHTUSD_BUILD_GUI_VIEWER=ON` and deliberately uses the
same executable image: renderer, UsdSkel evaluation, blendshape deformation,
MaterialX/OpenPBR materials, texture cache, and BasisCurves hair stay identical.

```sh
./build_ninja/vchar --backend gl character.usdz
./build_ninja/vchar --backend vk character.usdz
./build_ninja/vchar --backend vk --rt character.usdz
./build_ninja/vchar --mcp-http 8765 character.usdz
./build_ninja/vchar --autorigger /path/to/lightrig character.usdz
```

The profile accepts USD, USDA, USDC and packaged USDZ input. Skeleton debug is
enabled initially. OpenGL and Vulkan raster are production paths; Vulkan ray
query is the native RT preview. CUDA and HIP are named capability slots but are
disabled in this profile until their virtual-human material/deformation parity
is validated.

## Facial-first control and debug API

The embedded MCP server adds:

- `vchar_status`
- `list_blendshapes`, `set_blendshape_weights`
- `list_facial_controls`, `set_facial_controls`
- `vchar_debug` (`skin-weights`, `blend-influence`, normals, tangents, skeleton)
- `vchar_skin_profile` (tuned OpenPBR SSS/specular/fuzz/coat skin profile)
- `vchar_deformer` (UsdSkel status, `autorig`, and overlay application)
- `vchar_physics` (simulation plus body/collider/joint/contact diagnostics)
- `autorigger_inspect`, `apply_rig_overlay`

Without authored metadata, named facial controls map directly to USD blendshape
names. A prim can instead author parallel arrays in `customData`:

```usda
customData = {
    dictionary vchar = {
        string[] controlNames = ["jawOpen"]
        string[] controlMappings = ["jaw_open"]
        float2[] controlRanges = [(0, 1)]
        float[] controlDefaults = [0]
    }
}
```

The first authored control map found in stage traversal order is authoritative.
Missing mapping/range/default entries fall back to the control name, `[-1,1]`,
and zero. The dedicated Facial Rig panel uses the authored labels, limits, and
defaults directly; hovering a control shows its mapped USD blendshape. UsdSkel
joint animation and blendshapes execute in realtime.

`vchar_physics` initializes and steps LightUSD's bounded rigid-body solver from
authored UsdPhysics APIs. Animated kinematic prim transforms are
copied into their solver proxies before every step; dynamic solver transforms
are then written back to matching prims in an independent editable core
preview stage and to renderer mesh-world buffers. The document snapshot stays
immutable; reset restores its authored pose.
Proxy linear and angular velocities are derived from consecutive poses, so
animation-driven colliders transfer motion through contacts instead of behaving
like teleported static geometry.
This makes attachments follow a skinned/animated character while retaining
two-way coupling for simulated rigid pieces. `status`, `initialize`, `step`,
`reset`, and `hide` use the default core loader.

Physics diagnostics draw awake bodies as orange crosses (sleeping bodies are
dimmed), collider AABBs in cyan, joint anchors and axes in magenta, and contact
normals in red. The geometry and solver overlays remain visible together for
spotting incorrect proxy transforms, joint frames, and penetration. Mesh
colliders use conservative local bounds, scanned with fixed scratch storage
when no extent is authored. Unsupported geometry, shear and reflections
return an explicit diagnostic error. This preview is not an engine-exact
MuJoCo/PhysX replacement.

## External auto-rigger boundary

The external project is a persistent JSON-RPC 2.0 worker over stdin/stdout. Each
line is one UTF-8 JSON message; stdout must contain protocol messages only.
Supported methods are `rig.initialize`, `rig.inspect`, `rig.submit`,
`rig.status`, and `rig.cancel`, plus optional LightRig DNA methods
`dna.inspect`, `dna.convert`, and `dna.evaluate`. `dna.evaluate`
accepts a selected LOD and either raw DNA or canonical facial controls,
returning joints, blendshapes, and animated maps for playback clients.
`rig.submit` receives the source asset path,
facial-first options, and an output-layer path. Its authoritative result is a
USDA or USDC overlay, never a rewritten character asset.

Compose the completed layer with `apply_rig_overlay`. vchar creates a temporary
session root whose sublayers are the overlay and original asset, then reloads
that root. This keeps the source USDZ untouched and makes auto-rig results easy
to replace or discard. See [autorigger-protocol.schema.json](autorigger-protocol.schema.json)
for the request/result shape.

`vchar_deformer` exposes the active in-process adapters and their evaluation
order. The `usdskel` adapter evaluates blendshapes/inbetweens and skinning; the
`usd-overlay` adapter adds controls, correctives, and physics metadata without
modifying the source character. Select the external JSON-RPC launcher with
`--autorigger`; the `autorig` operation applies its completed overlay through
the same adapter path and reports bounded timeout or worker-exit failures.

Hair uses the shared BasisCurves ribbon rasterizer (and tube proxies for Vulkan
RT), including authored widths and curve tangents. Skin uses the shared
MaterialX/OpenPBR multilobe model. `vchar_skin_profile` tunes subsurface color,
radius, and scale together with dual specular, fuzz, and coat response; raster
uses the viewer's realtime SSS approximation while RT evaluates the same
material data through LightRT. Omit `material_id` to select the strongest
face/skin/head/body material-name match automatically. Explicit OpenPBR and the
shared realtime-PBR fallback are both accepted, which covers converted
UsdPreviewSurface/MaterialX assets without discarding their texture inputs.

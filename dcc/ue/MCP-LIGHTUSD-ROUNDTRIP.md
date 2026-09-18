# LightUSD UE MCP roundtrip procedure

This procedure uses the experimental Unreal Engine 5.8 MCP HTTP server and the
LightUSDUE plugin's own MCP tool. It is intended for the Linux source build
used by this project.

## Windows UE 5.8 MCP

The same workflow works with UE running on Windows and the client running on
Linux. No manual editor interaction is required when the project already has
the MCP plugin enabled. Add `ModelContextProtocol` to the project plugin list,
then put the settings in the editor's generated per-project location (not the
source `Config` directory):

```text
<Project>/Saved/Config/WindowsEditor/EditorPerProjectUserSettings.ini
```

```ini
[/Script/ModelContextProtocolEngine.ModelContextProtocolSettings]
bAutoStartServer=True
ServerPortNumber=8000
ServerUrlPath=/mcp
bEnableToolSearch=True
```

Launch `UnrealEditor.exe` with `-ModelContextProtocolStartServer` and, for a
headless automation run, `-nullrhi -NoUBA`. The Windows package must contain
the LightUSD runtime DLL beside `UnrealEditor-LightUSDUE.dll`; the MinGW-built
module imports it as `liblightusd_c.dll`, so retain that exact filename (the
other runtime DLLs such as `libc++.dll`, `libunwind.dll`, and
`libwinpthread-1.dll` must be colocated there as well).

From Linux, use an SSH local forward when TCP forwarding is permitted:

```sh
ssh -N -L 18000:127.0.0.1:8000 <windows-host>
```

Then send the normal MCP `initialize`, `notifications/initialized`, and
`tools/list` requests to `http://127.0.0.1:18000/mcp`. If the Windows SSH
service disallows local forwarding, issue the same HTTP requests with
PowerShell `Invoke-WebRequest` (or `curl.exe`) on the Windows host through
SSH. The protocol and session handling are unchanged.

The verified Windows path exposed `LightUSDRunPython`; it exported the UE
built-in cube with the native USD exporter, validated the USDA with LightUSD,
and imported it with the native UE USD importer, creating `SM_Cube` and
`MI_DisplayColor`. A MetaHuman identity-template path was also queried and
correctly reported missing when the project did not contain the local
MetaHuman template assets; EOS/MetaHuman Creator authorization is not needed
for this basic mesh round-trip.

For a fresh Windows UE project with `MetaHumanCharacter` enabled, run
`dcc/ue/tests/ue_metahuman_template_scene_roundtrip.py` with
`UnrealEditor-Cmd.exe -ExecutePythonScript`. The script spawns face and body
skeletal actors from the local identity templates, saves
`/Game/LightUSD/MetaHumanTemplateScene`, exports `face.usda` and `body.usda`,
validates both with LightUSD, imports through the LightUSD backend, and also
checks native UE import. The verified UE 5.8 run used no EOS and no
auto-rigging: LightUSD reported 30 face prims and 5 body prims, while native
import created skeletal meshes, skeletons, physics assets, and materials for
both templates.

## 1. Enable the UE MCP server

Add the following project plugin entry to `MhUsdTest.uproject`:

```json
{ "Name": "ModelContextProtocol", "Enabled": true }
```

Add `Config/DefaultEditorPerProjectUserSettings.ini`:

```ini
[/Script/ModelContextProtocolEngine.ModelContextProtocolSettings]
bAutoStartServer=True
ServerPortNumber=8000
ServerUrlPath=/mcp
bEnableToolSearch=True
```

The built-in UE server is available at `http://127.0.0.1:8000/mcp`.

## 2. Build the LightUSDUE MCP bridge

The plugin declares `ModelContextProtocol` and registers the native
`LightUSDRunPython` tool. Build with UBA disabled for the UE 5.8 shared-PCH
rename issue:

```sh
/mnt/disk1/local/ue/Engine/Build/BatchFiles/Linux/Build.sh \
  UnrealEditor Linux Development \
  -Project=/mnt/disk1/work/ue/MhUsdTest/MhUsdTest.uproject \
  -Plugin=/mnt/disk1/work/ue/MhUsdTest/Plugins/LightUSDUE/LightUSDUE.uplugin \
  -NoHotReloadFromIDE -NoUBA
```

## 3. Launch a headless MCP editor

Use another port when a normal editor is already running:

```sh
/mnt/disk1/local/ue/Engine/Binaries/Linux/UnrealEditor \
  /mnt/disk1/work/ue/MhUsdTest/MhUsdTest.uproject \
  -nullrhi -unattended -NoSplash -NoSound -NoP4 \
  -ModelContextProtocolStartServer -ModelContextProtocolPort=8001 \
  -stdout -FullStdOutLogOutput
```

`-unattended` suppresses modal dialogs. `-nullrhi` is suitable for USD and
asset automation, but MetaHuman rendering features that require a supported
GPU RHI are unavailable.

## 4. MCP session handshake

POST `initialize` to `/mcp`, retain the returned `Mcp-Session-Id`, then POST
`notifications/initialized`. Request `tools/list` to confirm both the UE
discovery tools and `LightUSDRunPython` are registered.

The custom tool accepts a Python script in its `code` parameter. Set the
global `_lightusd_mcp_result` to a JSON string to return structured text to the
MCP caller.

## 5. LightUSD roundtrip order

For a simple test:

1. Create/select a mesh actor and assign a material in the editor world.
2. Call `lightusd_ue.export_usd(..., backend="native")` through
   `LightUSDRunPython`.
3. Call `lightusd_ue.validate_usd(...)` using LightUSD.
4. Call `lightusd_ue.import_usd(..., backend="native", package_path=...)`.
5. Return the USD path, validation result, imported asset paths, and warnings.

For headless UE, prefer direct asset export for the first test:

```python
task = unreal.AssetExportTask()
task.object = unreal.load_object(None, "/Engine/BasicShapes/Cube.Cube")
task.filename = "/tmp/lightusd_mcp_staticmesh.usda"
task.options = unreal.StaticMeshExporterUSDOptions()
unreal.Exporter.run_asset_export_task(task)
```

The level exporter can traverse the default OpenWorld template even when an
actor is selected. Under `-nullrhi`, its landscape-material path can also hit
an engine ensure/crash. Direct static/skeletal asset export avoids that path.

## Verified through the custom MCP tool

The headless editor was launched on port 8002 and the following calls passed:

* Simple mesh: `/Engine/BasicShapes/Cube.Cube` exported to
  `/tmp/lightusd_mcp_staticmesh.usda`. LightUSD found `/Cube`,
  `/Cube/UnrealMaterial`, and `/Cube/UnrealMaterial/UnrealShader`; native
  import created `SM_Cube` and `MI_DisplayColor`.
* MetaHuman body template:
  `/MetaHumanCharacter/Body/IdentityTemplate/SKM_Body` exported to
  `/tmp/lightusd_mcp_metahuman_body.usda`. LightUSD found the mesh, LOD,
  material/shader, and skeleton prims; native import created the skeletal mesh,
  skeleton, physics asset, and material instance under
  `/Game/LightUSD/MCPMetaHumanBody`.

Both calls used `LightUSDRunPython` over MCP; export used UE's native USD
exporter, validation used LightUSD, and import used UE's native USD importer.

### Material graph result

The MCP test also created `/Game/LightUSD/MCPFeatures/M_MCPNodeGraph` with
five UE material expressions (two `Constant3Vector` nodes, a
`LinearInterpolate`, and two scalar constants), assigned it to a duplicated
Cube, and exported/imported the mesh successfully:

* LightUSD validation passed and reported the mesh, material, and shader prims.
* Native import created `SM_MCPNodeGraph` and `MI_DisplayColor` with no USD
  import warnings.
* UE's native USD exporter preserved the material assignment as
  `info:unreal:sourceAsset`, but did not serialize the UE expression graph as
  USD shader nodes. The imported material is therefore a generated
  `MaterialInstanceConstant`, not a reconstruction of the five-node graph.

The LightUSDUE bridge now has a material-specific path for preserving the
editable UE graph in addition to the portable MaterialX/OpenPBR representation.
It is exposed through the Python facade:

```python
import lightusd_ue.api as api

api.export_material(
    unreal.load_object(None, "/Game/LightUSD/MCPFeatures/M_MCPNodeGraph"),
    "/tmp/lightusd_mcp_full_material.usda",
    preserve_ue_config=True,
    prefer_materialx=True,
)
api.import_material(
    "/tmp/lightusd_mcp_full_material.usda",
    "/Game/LightUSD/MCPMaterialBridge",
)
```

The exported Material applies both `MaterialXConfigAPI` and
`MaterialUEConfigAPI`. Portable nodes are represented by an OpenPBR surface;
UE expressions are represented as typed `Shader` prims under `UEGraph`, using
`info:id = "UnrealMaterialExpression.<class>"`. UE class paths, editor layout,
connections, and a JSON property archive are preserved. The extended import
regression recreates all 14 test expressions and their classes through the
same bridge.

Common UE expressions also receive a portable MaterialX representation:
`Constant`, `Constant3Vector`, `TextureSample`, `TextureCoordinate`,
`LinearInterpolate`, and basic arithmetic nodes are emitted with `ND_*`
`info:id` values in `MaterialXGraph`. The UE graph remains alongside it for
lossless fallback. Canonical OpenPBR inputs are reconnected on import, and
texture paths stored in the UE archive are resolved back to UE `Texture2D`
assets when available.

The UE 5.8 extended regression also covers scalar/vector/static parameters,
static switches, normalize/normal chains, runtime virtual-texture samples,
material-function calls, Make/Blend Material Attributes, and Clear Coat
shading-model restoration. Make/Set Material Attributes export as
`ND_open_pbr_surface_surfaceshader`; Blend Material Attributes exports as
`ND_mix_surfaceshader`, with UE pins translated to OpenPBR and surface-mix
inputs. Recognized material functions also produce a sibling
`.functions.mtlx` library containing a real MaterialX `nodedef` and
`nodegraph`; the shader's `info:sourceAsset` points at that implementation.
`MakeFloat3` currently translates to a `combine3` graph. Unsupported functions
are marked `lightusd:functionStatus = "ue-only"` instead of claiming portable
behavior. The original object path remains in `unreal:functionAsset`.

Break/Get Material Attributes retain an extraction semantic. UE 5.8 Substrate
Slab/Shading Models map to OpenPBR, Horizontal Mixing maps to
`ND_mix_surfaceshader`, and Vertical Layering maps to
`ND_layer_surfaceshader`. Operators without a faithful MaterialX equivalent
remain UE-only. The adjacent `UEGraph` stays authoritative for exact UE
reconstruction.

`MaterialUEConfigAPI` is the explicit fallback for UE-only settings and future
engine-specific properties. It is intentionally additive: consumers that do
not understand it can still use the MaterialX/OpenPBR graph, while UE can
reconstruct the original expression graph when the archive is present.

An existing MaterialX USD fixture (`models/cube-materialx.usda`) was also
validated and imported through the same MCP bridge. LightUSD recognized 15
prim paths, including the MaterialX config, surface shader, and six nodegraph
nodes. UE native import succeeded and created `SM_Cube` plus two material
instances. The native importer therefore handles the fixture without errors,
but the returned assets are material instances rather than a UE expression
graph reconstruction.

For MetaHuman data, repeat the same sequence with the body/face skeletal mesh,
then add the MetaHuman asset path and enable physics, groom, and relinking
options. Auto-rigging is not required for the template roundtrip.

### Windows UE <-> Linux Blender asset bridge

When the two MCP endpoints cannot directly share a filesystem, use the
dependency-free base64 bridge in `dcc/bridge/asset_bridge.py`. Start it on
the Linux host, upload a Blender USDA, then run the UE-side launcher on
Windows:

```sh
python3 -m dcc.bridge.asset_bridge --root /tmp/lightusd-bridge-live \
  --host 0.0.0.0 --port 8765 --token bridge-test-20260918
```

```powershell
powershell -ExecutionPolicy Bypass -File dcc/ue/tests/run_asset_bridge.ps1 `
  -BridgeUrl http://<linux-host>:8765 `
  -BridgeToken bridge-test-20260918 -AssetId <blender-upload-id>
```

The UE script downloads the asset, imports it through the native UE backend,
exports the first skeletal mesh, validates that export with LightUSD, and
uploads the result. The report is written to
`D:\work\lightusd\UBTFullTest\BlenderBridge\report.json`. The 2026-09-18
live test passed with UE 5.8: native import created a skeletal mesh, skeleton,
physics asset, and material; LightUSD counted 6 output prims; Blender MCP
re-imported the returned USDA with one armature, one skinned mesh, and two
bones.

For a single machine-readable run, use `dcc/run_interop_regressions.py` with
`dcc/interop-regression.example.json`. The runner starts the local bridge,
runs the Blender 5.2 regression matrix, uploads a checksummed dependency
bundle, establishes configured background transports such as an SSH reverse
tunnel, invokes UE 5.8, and embeds UE's verified JSON result in its own report.
Commands are argument arrays rather than shell strings, and bridge tokens are
redacted from the report.

Dependency bundles use `lightusd-asset-bundle-v1`: every USDA/USDC layer,
MaterialX document, texture, and UDIM tile has a relative path, byte size, and
SHA-256 entry. Both the desktop and UE clients reject absolute paths, parent
traversal, drive names, checksum mismatches, and oversized expanded bundles.

`upload_asset_bundle_http()` and `dcc.bridge.transfer.upload_asset_bundle()`
discover the closure automatically. They recursively follow `@asset@`
references in USD layers, `file`/`filename`/`sourceuri`/`href` attributes in
MaterialX documents, and expand every matching `<UDIM>` tile. Discovery stays
inside the selected bundle root and is strict by default: missing files, root
escapes, and malformed containers fail before upload. Binary USDC is decoded
through LightUSD without composition, preserving discoverable sublayers,
references, payloads, value clips, and asset properties. USDZ members are
inspected in place; packaged files stay in the archive and external references
are added to the transfer bundle.
Explicit `bundle_dependencies` remain available in the unified runner for
generated files that are intentionally not referenced by the root layer.

Windows can run the complete UE-only matrix directly:

```powershell
powershell -ExecutionPolicy Bypass -File dcc/ue/tests/run_headless_regressions.ps1 `
  -Editor "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  -Project D:\work\lightusd\UBTFullTest\UBTFullTest.uproject `
  -RepoRoot D:\work\lightusd -OutRoot C:\tmp\lightusd-regressions -MetaHuman
```

The suite requires a fresh `report.json` from every editor process and writes
an aggregate `summary.json`. Without `-MetaHuman` it covers physics, extended
materials, rigged mesh/animation, UsdSkel facial blendshapes, static and
animated groom, NURBS, guide-only groom, and groom cards. `-MetaHuman` adds
template, material/UDIM, and scene roundtrips.

### UE 5.8 Windows verification (2026-09-18)

The selected regression targets were run against the Windows UE 5.8 editor
through the headless commandlet/MCP automation path:

| Target | Result |
| --- | --- |
| Material graph | 14 UE expressions reconstructed; Base Color, parameters, function/layer nodes, virtual texture and Clear Coat settings restored; LightUSD validation counted 29 prims. |
| Groom | Static and animated BasisCurves imported as GroomAsset + GroomBindingAsset + GroomCache + hair material, exported, and re-imported. The NURBS fixture was tessellated and retained the required warning. |
| UE ↔ Linux bridge | Native UE import created skeletal mesh, skeleton, physics asset, and material; UE export uploaded successfully; LightUSD validation counted 6 prims. |

These runs produced reports under the UE test project's `Regressions` and
`BlenderBridge` folders. The only command-line warnings were UBT SDK checks
for non-Win64 target platforms; they did not affect the Win64 editor runs.

### Repeatable material regression

The MCP result is also covered by a headless UE script:

```sh
/mnt/disk1/local/ue/Engine/Binaries/Linux/UnrealEditor-Cmd \
  /mnt/disk1/work/ue/MhUsdTest/MhUsdTest.uproject \
  -ExecutePythonScript=/mnt/nvme02/work/tinyusdz-repo/dev/dcc/ue/tests/ue_material_graph_roundtrip.py \
  -unattended -nullrhi -NoSplash -NoSound -NoP4 -NoUBA
```

It writes `/tmp/lightusd_ue_material_graph/report.json` and verifies the
MaterialX graph, UE expression count, Base Color reconnection, and texture
restoration.

### Repeatable MetaHuman template and UDIM regression

The local MetaHuman Character plugin provides face and body identity-template
meshes even without EOS authorization. The following headless test exports both
skeletal meshes, validates them with LightUSD, and imports them with physics:

```sh
/mnt/disk1/local/ue/Engine/Binaries/Linux/UnrealEditor-Cmd \
  /mnt/disk1/work/ue/MhUsdTest/MhUsdTest.uproject \
  -ExecutePythonScript=/mnt/nvme02/work/tinyusdz-repo/dev/dcc/ue/tests/ue_metahuman_template_roundtrip.py \
  -unattended -nullrhi -NoSplash -NoSound -NoP4 -NoUBA
```

It writes `/tmp/lightusd_ue_metahuman_templates/report.json`. The verified
native assets include a skeletal mesh, skeleton, physics asset, and material
instance for both face and body.

The body material/UDIM test additionally generates two PNG tiles, exports the
body mesh and material, validates a `UsdUVTexture` asset path containing
`<UDIM>`, and imports both the native UE USD material and a LightUSD-authored
preview-surface material:

The UE-side tile import assigns explicit destination names (`T_*_1001` and
`T_*_1002`). This is required because UE's dotted filename normalization can
otherwise strip the UDIM suffix and collapse both source files to one Content
Browser asset. The native USD material importer continues to create its
consolidated UDIM texture asset from the `<UDIM>` path.

The bound-material fixture `tests/usda/udim-material-bound.usda` verifies the
full dependency case: UE creates and saves `T_lightusd_skin`, and a fresh UE
process reloads the imported material instance with a `BaseColorTexture`
parameter referencing that texture asset.

```sh
/mnt/disk1/local/ue/Engine/Binaries/Linux/UnrealEditor-Cmd \
  /mnt/disk1/work/ue/MhUsdTest/MhUsdTest.uproject \
  -ExecutePythonScript=/mnt/nvme02/work/tinyusdz-repo/dev/dcc/ue/tests/ue_metahuman_material_roundtrip.py \
  -unattended -nullrhi -NoSplash -NoSound -NoP4 -NoUBA
```

It writes `/tmp/lightusd_ue_metahuman_materials/report.json`; the latest run
verified five LightUSD prims, three UDIM material prims, and native skeletal,
skeleton, physics, material, and texture assets. Groom coverage is exercised
separately by the BasisCurves test below.

### LightUSD BasisCurves groom import

The Blender bridge exports static Hair Curves as USD BasisCurves with
non-periodic linear strands, widths, group IDs, guide flags, strand IDs, and
root UVs, per-point color, and per-point roughness. UE's LightUSD backend
translates these arrays directly into an FHairDescription, imports a native UGroomAsset, and can create a
UGroomBindingAsset when groom_target_skeletal_mesh_path is supplied.

Generate a deterministic Blender fixture at /tmp/lightusd_blender_groom.usda;
the checked-in headless UE test consumes that file:

    /mnt/disk1/local/ue/Engine/Binaries/Linux/UnrealEditor-Cmd \
      /mnt/disk1/work/ue/MhUsdTest/MhUsdTest.uproject \
      -ExecutePythonScript=/mnt/nvme02/work/tinyusdz-repo/dev/dcc/ue/tests/ue_groom_roundtrip.py \
      -unattended -nullrhi -NoSplash -NoSound -NoP4 -NoUBA

The test writes /tmp/lightusd_ue_groom/report.json and verifies both the
native groom asset and its binding against
/MetaHumanCharacter/Face/SKM_Face.

The same test then exports the resulting UGroomAsset back to
/tmp/lightusd_ue_groom/ue_groom_export.usda, validates /World/Groom, and
imports it again with a second binding. The export is required to retain the
color and roughness primvars. This is the supported static groom round-trip;
Blender animated curve objects author `points` and `widths` time samples and
preserve the stage frame range. The LightUSD UE adapter now builds a native
`UGroomCache` from those samples, validates fixed topology, and keeps the first
sample as the static groom source. It records position, width, and color cache
attributes plus stable group, guide, strand-ID, and root-UV topology metadata.
`export_groom` exports a static `UGroomAsset`. For animation, use
`export_groom_cache(cache, groom_asset, filename)`: the cache supplies animated
point/radius samples while the source `UGroomAsset` supplies stable curve
topology. The resulting LightUSD `BasisCurves` carries `points.timeSamples` and
`widths.timeSamples` and can be imported back into a native `UGroomCache`.

`NurbsCurves` are supported in the LightUSD path by tessellating the USD NURBS
control points, knots, order, ranges, and optional weights to linear
HairStrands curves. The Blender bridge can export/import NURBS splines. Groom
cards are represented as ordinary USD `Mesh` prims with `groom_card` and
`groom_card_id` metadata, so they retain geometry, materials, and card identity
through the existing mesh path. On UE import, LightUSD remains authoritative
for strands and caches while card Mesh prims are handed to UE's native USD
geometry importer and become native StaticMesh assets.

Guide-only BasisCurves require render strands internally in UE HairStrands.
The adapter promotes them internally, marks the groom package with their
original role, and restores `primvars:groom_guide = 1` on static and animated
USD export. This path is covered by `tests/usda/blender-guide-groom.usda`.

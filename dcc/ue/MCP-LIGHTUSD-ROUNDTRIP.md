# LightUSD UE MCP roundtrip procedure

This procedure uses the experimental Unreal Engine 5.8 MCP HTTP server and the
LightUSDUE plugin's own MCP tool. It is intended for the Linux source build
used by this project.

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
connections, and a JSON property archive are preserved. The corresponding
import path recreated all five test expressions and their classes through the
MCP bridge.

Common UE expressions also receive a portable MaterialX representation:
`Constant`, `Constant3Vector`, `TextureSample`, `TextureCoordinate`,
`LinearInterpolate`, and basic arithmetic nodes are emitted with `ND_*`
`info:id` values in `MaterialXGraph`. The UE graph remains alongside it for
lossless fallback. Canonical OpenPBR inputs are reconnected on import, and
texture paths stored in the UE archive are resolved back to UE `Texture2D`
assets when available.

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
/MetaHumanCharacter/Face/SKM_Face. Groom caches, animation, cards, and NURBS
curves remain outside this static BasisCurves path.

The same test then exports the resulting UGroomAsset back to
/tmp/lightusd_ue_groom/ue_groom_export.usda, validates /World/Groom, and
imports it again with a second binding. The export is required to retain the
color and roughness primvars. This is the supported static groom round-trip;
Blender animated curve objects author `points` and `widths` time samples and
preserve the stage frame range. The LightUSD UE adapter now builds a native
`UGroomCache` from those samples, validates fixed topology, and keeps the first
sample as the static groom source. It records position, width, and color cache
attributes; guide-only cache authoring remains a separate extension.
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
through the existing mesh path.

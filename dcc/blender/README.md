# LightUSD Blender bridge

This Blender 5.2+ add-on provides one import/export UI for Blender OpenUSD,
LightUSD, and the native OpenUSD hook path. The LightUSD backend does not need
`pxr`; set **Preferences > Add-ons > LightUSD USD Bridge > LightUSD Python path**
to a directory containing the `lightusd` package and its abi3 extension.

The LightUSD path covers composed stages, meshes, transforms, cameras, lights,
hair curves, MaterialX/OpenPBR-compatible graphs, Blender rigid bodies, and
preservation of `mjc:*` and `newton:*` physics attributes. Native OpenUSD keeps
Blender's importer/exporter and installs a hook class for future scene and
material extensions.

For development from this repository, use `LIGHTUSD_PYTHON_PATH` or point the
preference at `python/` plus the matching `build/lib.*` extension directory.

The checked-in curve regression can be run headlessly with Blender 5.2:

```sh
"${BLENDER:-blender}" \
  --background --python dcc/blender/tests/roundtrip.py
```

It imports and re-exports a NURBS curve and an animated BasisCurves groom,
checking topology and both point and width time samples.

## UE skeleton and Rigify interchange

`converter.py` maps USD `SkelRoot`/`Skeleton`/skinned `Mesh` prims to a Blender
armature, vertex groups, and an Armature modifier. Export writes the UE joint
paths, rest and bind transforms, four normalized influences per vertex, and
`unreal:*`/`rigify:*` metadata into the USD layer. The UE source skeleton is
canonical, so Blender bone display names may be changed without breaking the
mapping. `SkelAnimation` translation, quaternion rotation, and scale samples
are exchanged as Blender Actions using the same joint order. USD `BlendShape`
targets and `blendShapeWeights` are exchanged as Blender shape keys and their
animation data, which provides the facial-expression path needed by
MetaHuman-style rigs.

LightUSD uses Blender 5.2+'s native Rigify add-on. `dcc/blender/rigify.py`
records the canonical UE mannequin map and the stable
`LightUSD.UE.Rigify.v1` retarget profile as USD metadata. The headless
regression is:

```sh
LIGHTUSD_PYTHON_PATH="$PWD/python" \
  "${BLENDER:-blender}" \
  --background --python dcc/blender/tests/usdskel_roundtrip.py
```

`dcc/blender/rigify.py::retarget_action` copies pose-bone F-curves from the UE
names to the canonical Rigify deform names and leaves unrecognized control-rig
channels untouched rather than guessing. Its regression is:

```sh
"${BLENDER:-blender}" \
  --background --python dcc/blender/tests/rigify_retarget.py
```

`dcc/blender/tests/rigify_metarig.py` additionally generates Blender's
built-in metarig and Rigify deformation rig, then verifies all 23 mapped UE
targets exist on the generated armature.

Enable Blender's built-in Rigify add-on before running the headless tests. The
supported interchange target is UE 5.8+ and Blender 5.2+.

The native Blender 5.2 regression matrix has been verified: UE face and body
template exports round-trip with 875 and 342 bones respectively, both with a
skinned mesh; NURBS and animated groom curves preserve their topology and time
samples; and the MaterialX channel/unsupported-node regression passes.

## MaterialX channel coverage and diagnostics

The Blender exporter maps the extended Principled/OpenPBR channel set
(specular weight/IOR, coat, sheen, anisotropy, transmission, subsurface,
emission, opacity, and the existing base/roughness/metalness channels). It
also preserves the original Blender node inventory and writes
`userProperties:lightusd:unsupportedNodes` on the OpenPBR shader for node types
that cannot be translated. Unsupported nodes remain reviewable instead of
silently becoming a different graph. The focused regression is:

```sh
"${BLENDER:-blender}" \
  --background --python dcc/blender/tests/materialx_channels.py
```

Exported bound meshes are nested below their `SkelRoot` and carry
`SkelBindingAPI`, so the generated USDA can be imported by UE as a skeletal
mesh rather than a static mesh. The UE-side smoke test confirmed one skeleton,
one physics asset, and one morph target from the Blender-generated layer.

## UE Windows <-> Blender Linux MCP round-trip

The installed Blender 5.2 MCP extension can be started headlessly without
opening the UI:

```sh
LIGHTUSD_PYTHON_PATH="$PWD/python" \
  "${BLENDER:-blender}" \
  --background --command blender_mcp --host 127.0.0.1 --port 9876
```

The Blender MCP bridge uses a localhost TCP socket with null-byte-delimited
JSON requests. A request has the form
`{"type":"execute","strict_json":true,"code":"..."}` and the executed
code must assign a JSON-serializable `result` dictionary. This is separate
from UE's HTTP MCP endpoint: use the Blender socket for Blender Python and the
UE `/mcp` HTTP endpoint for `LightUSDRunPython`.

The checked-in MCP cross-DCC fixtures are:

```text
dcc/blender/tests/mcp_ue_skinned_export.py
dcc/blender/tests/mcp_ue_skinned_import.py
```

The verified sequence was Blender MCP export -> Windows UE MCP native import
-> Windows UE MCP skeletal USD export and LightUSD validation -> Blender MCP
import. UE created a skeletal mesh, skeleton, physics asset, and material;
Blender recovered one armature, one mesh, one armature modifier, and two
bones. This is the deterministic interchange smoke test before applying the
same path to larger MetaHuman face/body template exports.

### Base64 asset-transfer fallback

When the UE and Blender MCP clients cannot share a filesystem or SSH local
forwarding is unavailable, run the dependency-free bridge on either host:

```sh
python3 -m dcc.bridge.asset_bridge \
  --root /tmp/lightusd-bridge --host 0.0.0.0 --port 8765 \
  --token "<shared-secret>"
```

The HTTP API is `POST /v1/upload` and `GET /v1/download/<sha256>`. Requests
carry JSON with base64 `data`, `name`, and optional `sha256`, and authenticate
with `X-LightUSD-Bridge-Token`. `/health` is available for connectivity
checks. The same upload/download messages are supported by the WebSocket
endpoint `GET /v1/ws`; use this only on a trusted network and always set a
token when binding beyond localhost.

The store is content-addressed, rejects path traversal, verifies SHA-256, and
enforces a configurable decoded-size limit. `dcc.bridge.asset_bridge` also
exports `upload_http()` and `download_http()` for small UE/Blender automation
scripts.

For DCC scripts, use the file helpers so the bridge handles base64 encoding,
checksums, and atomic writes:

```python
from dcc.bridge.transfer import download_file, upload_file

asset = upload_file("http://bridge-host:8765", "/tmp/scene.usda", "<shared-secret>")
download_file("http://bridge-host:8765", asset["id"], "/tmp/scene-copy.usda",
              "<shared-secret>")
```

Use `upload_asset_bundle()` for a complete scene transfer. It discovers text
USD and binary USDC composition/asset references, USDZ external references,
MaterialX includes and textures, and all matching `<UDIM>` tiles. Discovery is
confined to the supplied root and the resulting ZIP manifest checksums every
relative file. Cycles terminate safely; configurable file-count and byte-size
limits bound closure traversal, and case-only path collisions are rejected for
portable extraction on Windows.

The verified Windows UE 5.8 fallback test uses the checked-in launch script:

```powershell
python3 -m dcc.bridge.asset_bridge --root /tmp/lightusd-bridge-live \
  --host 0.0.0.0 --port 8765 --token bridge-test-20260918
pwsh dcc/ue/tests/run_asset_bridge.ps1 \
  -BridgeUrl http://<linux-host>:8765 \
  -BridgeToken bridge-test-20260918 -AssetId <blender-upload-id>
```

The live test uploaded a 2.5 KiB Blender skinned USDA, imported it in UE
5.8, exported a 3.8 KiB USDA, and uploaded it back. UE's LightUSD validator
reported 5 prims; Blender MCP re-imported the returned asset as one armature,
one skinned mesh, and two bones.

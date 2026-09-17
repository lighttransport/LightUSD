# LightUSD Unreal Engine bridge

This plugin adds USD import, export, and validation for Unreal Engine 5.6+
and newer. It supports three backends:

* `Auto` prefers Unreal's USD/Interchange implementation and uses LightUSD
  for validation and preservation passes.
* `NativeUE` uses the engine USD importer/exporters.
* `LightUSD` uses the dependency-free LightUSD C API and does not require
  Pixar USD or `pxr`.

The plugin is editor-only. Its C++ module is the stable integration layer;
`Content/Python/lightusd_ue` is a thin Unreal Python facade over the same API.

## Build/package

```sh
./dcc/ue/build_plugin.sh \
  --engine-source /mnt/disk1/work/ue/UnrealEngine \
  --engine-binary /mnt/disk1/local/ue/Engine \
  --project /mnt/disk1/work/ue/MhUsdTest \
  --install-engine /mnt/disk1/local/ue/Engine/Plugins/LightUSDUE \
  --install-project /mnt/disk1/work/ue/MhUsdTest/Plugins/LightUSDUE
```

The script builds LightUSD's `lightusd_c` static target and stages a plugin
package for each UE installation. It never modifies the Unreal Engine source.
For this local UE 5.8.2 Linux installation, compile the project with UBA
disabled because the installed UBA detour has a shared-PCH rename race:

```sh
/mnt/disk1/local/ue/Engine/Build/BatchFiles/Linux/Build.sh \
  UnrealEditor Linux Development \
  -Project=/mnt/disk1/work/ue/MhUsdTest/MhUsdTest.uproject \
  -Plugin=/mnt/disk1/work/ue/MhUsdTest/Plugins/LightUSDUE/LightUSDUE.uplugin \
  -NoHotReloadFromIDE -NoUBA
```

The same policy is available as a reproducible wrapper. It uses `-NoUBA` by
default and, when explicitly invoked with `--uba`, detects a shared-PCH rename
failure and retries once on the safe path:

```sh
./dcc/ue/build_editor.sh \
  --engine-binary /mnt/disk1/local/ue/Engine \
  --project /mnt/disk1/work/ue/MhUsdTest/MhUsdTest.uproject
```

Use `--uba` only after verifying the installed engine's UBA implementation;
the wrapper keeps the failed log and the retry log separately.

The packaging script builds the LightUSD archives with the UE x86_64
Clang/libc++ toolchain, PIC enabled, and stages all dependent archives.

## Windows x64 package

The Windows bridge uses a stable C-ABI DLL so the LightUSD implementation is
not linked as MinGW C++ objects into Unreal's MSVC/UBT module. Cross-build the
DLL and stage a complete UE plugin package with llvm-mingw:

```sh
LLVM_MINGW_DIR="$HOME/local/llvm-mingw-20260616-ucrt-ubuntu-22.04-x86_64" \
STAGE_DIR="$PWD/dist/lightusd-windows-x64" \
./dcc/build_windows_x64.sh
```

The script produces `bin/lightusd_c.dll` for Blender, C API headers and import
library, and `ue/` containing the source plugin, `ThirdParty/Win64`, and
`Binaries/Win64/lightusd_c.dll` plus its libc++ runtime DLLs. Build the UE module on Windows with UBT so it
uses the installed UE toolchain; the Linux cross-build intentionally does not
attempt to compile Unreal's MSVC-facing module. The script runs the C API
roundtrip executable under `wine64` when available, or the 64-bit `wine`
loader on distributions that do not install a separate `wine64` command.

For Blender on Windows, place `bin/lightusd_c.dll` beside the Python extension
or add its directory to the DLL search path before importing the LightUSD
Python bridge. The Python wheel itself should be built with the Windows CPython
headers/interpreter; the llvm-mingw build supplies the native LightUSD DLL,
not a Linux-hosted CPython extension.

## Python-first UE profile

When the UE-side C++ ABI should be minimized, use the staged
`ue-python-only/LightUSDUEPython.uplugin` profile. Its only native dependency
is Unreal's `PythonScriptPlugin`; it does not link `lightusd_c`, USD, HairStrands,
ControlRig, MCP, or any other UE feature module. Copy the Python content and
install the Windows LightUSD Python binding in the project environment, then
enable the plugin:

```python
import unreal
import lightusd_ue

result = lightusd_ue.import_usd(
    r"C:/assets/character.usdz",
    backend="lightusd",
    import_groom=True,
    import_physics=True,
    relink_metahuman=True,
)
unreal.log(str(result))
```

This profile is the recommended Windows Blender/UE interchange path when
native UE asset creation is not required. The full `LightUSDUE` plugin remains
available for native UE skeletal, groom, material, and MCP operations.

## Regression suite

The checked-in headless scripts cover the portable material graph, UDIM asset
paths, groom/static and animated BasisCurves, rigged USD skeleton data, and
physics schema preservation. Run the complete local suite after staging the
plugin into the project:

```sh
ENGINE_EDITOR=/mnt/disk1/local/ue/Engine/Binaries/Linux/UnrealEditor-Cmd \
PROJECT=/mnt/disk1/work/ue/MhUsdTest/MhUsdTest.uproject \
./dcc/ue/tests/run_headless_regressions.sh
```

The physics case uses `tests/usda/ue-physics-roundtrip.usda` and asserts that
the scene, rigid body, collider, material, and fixed-joint prims survive the
LightUSD import path. UE's native USD importer remains selectable with
`backend="native"`; schema-specific Chaos asset creation is intentionally
kept in the native importer rather than silently approximated by LightUSD.

The native UsdSkel smoke test can be run independently:

```sh
/mnt/disk1/local/ue/Engine/Binaries/Linux/UnrealEditor-Cmd \
  /mnt/disk1/work/ue/MhUsdTest/MhUsdTest.uproject \
  -ExecutePythonScript=/mnt/nvme02/work/tinyusdz-repo/dev/dcc/ue/tests/ue_usdskel_fixture.py \
  -unattended -nullrhi -NoSplash -stdout -FullStdOutLogOutput -NoSound -NoUBA
```

The default fixture is a single-character skinned mesh with one skeleton,
joint animation, and a facial blendshape. The larger multi-root fixture can be
selected with `LIGHTUSD_USDSKEL_FIXTURE` for schema coverage; it may import as
static meshes because it does not model a single UE skeletal asset package.

## Python

```python
import lightusd_ue

result = lightusd_ue.import_usd(
    "/tmp/character.usdz",
    backend="auto",
    import_groom=True,
    import_physics=True,
    relink_metahuman=True,
)
print(result.backend, result.created_assets)
```

The portable MetaHuman contract is UsdSkel, BasisCurves, and explicit
`unreal:*`, `metahuman:*`, `riglogic:*`, and `controlRig:*` metadata. Existing
UE assets are relinked; skeletal animation and facial blendshape animation
remain in the native UE USD path; proprietary UAF content is not reconstructed.

The TutorialTPP regression validates the exported animation layer with
LightUSD and successfully invokes UE native animation import. UE 5.8’s
standalone `SkelAnimation` import can still produce only a transient imported
actor, so the bridge exposes `create_skeletal_animation(...)`: a small C++
Blueprint-library shim around Epic’s non-reflected `UAnimSequence` controller.
It creates and saves a persistent `AnimSequence`, and the regression exports
that asset again and validates the resulting USD layer. The Python facade
also provides `compose_skeletal_animation(mesh_usd, animation_usd, output_usd)`
to author the standard `Skeleton.skel:animationSource` composition wrapper
for importers that require the animation and skeleton in one composed stage.

For a standalone USD animation layer, use
`import_skeletal_animation(filename, name, skeleton, package_path=..., preview_mesh=...)`.
This reads `SkelAnimation` joints and sampled rotations, translations, and
scales through LightUSD, canonicalizes joint names against the UE
`USkeleton`, saves a real `AnimSequence`, and leaves it ready for
`OverrideAnimationData` or `PlayAnimation`. The rigged regression covers this
real-data path, including reload, re-export, and playback assignment.

Facial animation is covered by `ue_usdskel_fixture.py`: USD
`blendShapeWeights` become UE morph-target float curves, and the exporter
post-processes the native layer when necessary so standard
`blendShapes`/`blendShapeWeights` fields survive the roundtrip.

## Headless MetaHuman generation

UE 5.8 exposes MetaHuman Character creation and assembly through Python. The
included script creates a character asset, seeds it from the installed face
and body identity templates, requests blocking auto-rigging, and assembles it:

```sh
/mnt/disk1/local/ue/Engine/Binaries/Linux/UnrealEditor-Cmd \
  /mnt/disk1/work/ue/MhUsdTest/MhUsdTest.uproject \
  -ExecutePythonScript=/mnt/nvme02/work/tinyusdz-repo/dev/dcc/ue/tests/ue_metahuman_generate.py \
  -unattended -nullrhi -NoSplash -stdout -FullStdOutLogOutput -NoSound -NoUBA
```

The blank character and template-conform steps are offline. Auto-rigging a new
character may require Epic account/device authorization and network access; a
headless run must provide that authorization before it can produce exportable
MetaHuman face rigs. The rigged USD roundtrip test uses `TutorialTPP` when no
assembled MetaHuman asset is available.

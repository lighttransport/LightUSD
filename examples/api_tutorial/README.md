# Core C++ API tutorial

Build from the repository root with `LIGHTUSD_BUILD_EXAMPLES=ON`, then run
`build_ninja/api_tutorial`. No input assets are required.

The example authors a material/shader network, an animated transform and a
mesh with indexed face-varying UVs using `LayerBuilder`. It then queries the
mesh through `UsdGeomMesh`, expands the UV indices with bounds checks,
evaluates the transform at an intermediate time, traverses the stage, and
checks that exported USDA reloads successfully. The exported scene is printed
to stdout. `core_api_tutorial` exercises the same path in CTest.

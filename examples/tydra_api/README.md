# Core Tydra scene queries

Build `tydra_api` with `LIGHTUSD_BUILD_EXAMPLES=ON` and `LIGHTUSD_WITH_TYDRA=ON`.
Run `build_ninja/tydra_api scene.usda` (USDC and USDZ also work).

The example composes references, finds meshes, transforms, materials and shaders,
queries parent prims and direct/inherited material bindings, resolves surface
shader terminals, and evaluates shader inputs with the core `AttributeEval` API.
Printed arrays are limited to 16 elements. The example returns nonzero on load
or composition failure.

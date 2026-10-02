# Checker definition provenance

`shaders.json` contains declarative port names and USD types exported with
`scripts/export-checker-shaders.py` from:

- OpenUSD `pxr/usd/plugin/usdShaders/shaders/shaderDefs.usda`, commit
  `2095fafafd033fa23386d7ec6d58c7cc33974518`.
- MaterialX 1.39.3 `libraries/stdlib/stdlib_defs.mtlx`,
  `libraries/bxdf/standard_surface.mtlx`, and
  `libraries/bxdf/open_pbr_surface.mtlx` in that reference installation.

No shader implementation code is included or executed. OpenUSD source is
licensed under its [license](https://openusd.org/license); MaterialX source
uses the [Apache-2.0 license](https://github.com/AcademySoftwareFoundation/MaterialX/blob/main/LICENSE).
The schema table uses the existing `doc/generated/openusd-schema-26.08.json`
manifest without dropping its newer definitions. Regenerate the C++ tables
with `python3 scripts/generate-checker-definitions.py`.

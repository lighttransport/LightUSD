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

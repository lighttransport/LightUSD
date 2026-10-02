# UDIM baking in usdzconvert

Folder conversion, streaming conversion, the CLI, and the browser Worker use
the same C++ atlas and direct-layer edit engine. Rebuild the combined and
next-only WASM products after changing that engine. Both wasm32 and memory64
are supported; the legacy WASM converter also exposes the bake API.

```js
const {usdz, stats} = await convertFolderToUSDZ(native, assets, {
  udimBake: 'dense',
  udimCrossTile: 'split',
  udimMaxTiles: 32,
  udimMaxAtlasSize: 8192,
  udimMemoryBudgetBytes: 512 * 1024 * 1024,
  udimDensePadding: 2,
  udimSubdivisionLevel: 2,
});
```

The next-only converter retains its single-root, asset-passthrough contract:
use `flatten: false`; external dependency-layer composition and variant
selection overrides require the combined product. Authored variants are
preserved and grid baking edits their texture definitions. Dense baking can
update authored prototype geometry consumed through internal references.
Instances that override prototype geometry require composition before dense
baking, so next-only conversion diagnoses them explicitly.

The browser converter exposes layout, tile cap, atlas edge, memory budget,
crossing-face policy, gutter, and subdivision controls. The CLI accepts
`--bake-udim`, `--udim-max-tiles`, `--udim-max-atlas-size`,
`--udim-memory-budget`, `--udim-cross-tile`, `--udim-dense-padding`, and
`--udim-subdivision-level`, matching the native flags.

Defaults are baking off, 100 tiles per layout, an 8192-pixel maximum edge,
512 MiB working memory, crossing faces rejected, 2-pixel dense gutters, and
2 subdivision levels. Supported markers are `<UDIM>`, `%04d`, and `%(UDIM)d`;
IDs range from 1001 through 9999. Excess tiles, missing tile sets, invalid
images, and budget/edge violations fail without dropping tiles. For animated
file opinions the cap counts the union across frames, and every frame uses the
same layout with absent tiles blank.

Grid keeps UDIM positions and adds a UV transform. Dense sorts IDs into compact
cells, reserves a blank cell, and adds a separate UV primvar/reader for each
texture layout. Existing UVs and non-UDIM textures keep their consumers.
Source resolution is preserved unless `maxTextureSize` requests resizing.
PNG8/PNG16 and EXR retain integer/floating-point precision. Resizing uses
linear-light filtering for sRGB integer inputs and premultiplied alpha. An
explicit JPEG request cannot discard 16-bit or floating-point precision.

Dense mode supports static clipping stencils applied to authored geometry
samples, skin weights, sparse blendshapes and their in-betweens, material
subsets, and subdivision. Discrete contributors must agree. Animated topology,
changing joint ownership, UV motion that changes ownership/clipping, and
animated UV-transform networks fail. The source layer is edited directly;
custom metadata and unrelated properties survive export.

Streaming sources can implement `fetch(key, {maxBytes})` to reject oversized
encoded tiles before reading them. The CLI and browser file sources do this.
Images are decoded one at a time. Streaming first validates and hashes each
atlas, then recreates it after emitting the root, checking the hash for source
changes. This bounds retained image memory at the cost of a second bake.
A source must therefore support repeatable reads. Folder conversion retains
encoded atlases, charges them to the budget, and avoids reprocessing their
source tiles. Concrete tile references used by other texture consumers remain
packaged.

The focused browser gate is `node tests/udim-bake-browser.test.mjs`; it verifies
controls, main-thread baking and legacy/next/streaming Workers.

Run `node tests/udim-bake.test.mjs` from `web/js`. Use
`LIGHTUSD_UDIM_COMBINED=1` for combined, `LIGHTUSD_WASM64=1` for memory64,
`LIGHTUSD_UDIM_GLUE` for an isolated module, and
`LIGHTUSD_NATIVE_USDZCONVERT` to include native USDA/USDC/USDZ CLI checks.
The Node regression profile includes the next-only bake suite. The fixture
creates its images in memory and leaves no binary assets in Git.

### Selected shader sets

`bakeLayerUDIM` accepts `udimShaderPaths: ['/World/Material/Texture']` to bake
only selected texture shaders. Its layer-edit request passes `shaderPaths` to
`applyUDIM`. Omit this option to retain whole-layer conversion. Selections must
be nonempty, unique, and known; plans must include every default and animated
file opinion of each selected shader. Plans outside the selection are rejected.
`describeUDIM().sites` also reports bound mesh/subset/reference consumers for
review. Existing unsupported-network/composition diagnostics still apply.

Lucia exposes this through an undoable preview/apply workflow in its Textures
inspector. `npm run test:lucia:browser` covers real worker discovery, grid/dense
stitching, viewport texture loading, atlas pixel preservation, USDZ packaging,
undo/redo, stale results, cancellation, and nested package roots.
Lucia requests optional `udimThumbnails` when baking. These display-only PNGs
are generated from resident atlas pixels in the shared C++ core, including
HDR/EXR inputs; floating-point previews use Reinhard tone mapping followed by
sRGB encoding. The encoded atlas retains its original precision. Thumbnail
scratch and retained bytes count toward the working-memory limit.

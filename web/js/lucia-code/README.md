# Lucia Code

Lucia Code is a browser-based USD scene editor backed by LightUSD and Three.js.
It opens directly on a working viewport with an editable USD hierarchy,
inspector controls, deterministic mesh operations, validation, exports, and an
offline assistant.

The proposed asset-conditioning architecture, feature priorities, and
implementation backlog are maintained in [DESIGN-TASKS.md](DESIGN-TASKS.md).

## Interaction and transaction guarantees

Lucia treats USD edits and generated project assets as one transaction. A
failed or cancelled operation restores the working USD, asset registry, export
remaps, and undo history. Successful edits invalidate validation until the
stage is checked again; baking, packing, resizing, and UV operations are also
undoable.

Validation shares the scene operation gate. Replacing the scene cancels any
pending validation result. Export requires successful validation; native
failure flags, malformed results, and validation exceptions block downloads,
while warnings alone remain exportable.

Export format selection and UV transfer use accessible in-app dialogs rather
than browser prompts. `Ctrl/Cmd+Z` and `Ctrl/Cmd+Y` mirror the toolbar undo and
redo actions when a text field or dialog is not active.

## Start locally

Prepare the generated LightUSD modules when they are not already present, then
start Vite from `web/js`:

```bash
bash ../demo/scripts/prepare-local-lightusd.sh
npm run dev:lucia
```

The default assistant is local and needs no account or network connection.
For optional OpenAI-compatible calls, run the proxy in another terminal:

```bash
LUCIA_LLM_API_KEY=... \
LUCIA_LLM_BASE_URL=https://api.openai.com/v1 \
LUCIA_LLM_MODEL=gpt-4.1-mini \
npm run proxy:lucia
```

Provider keys are read only by the Node proxy. They are never bundled into the
browser application or written to local storage.

## Verification

```bash
npm run test:lucia
npm run build
npm run test:lucia:browser
```

The browser smoke test reserves an ephemeral loopback port by default. Use
`LUCIA_SMOKE_PORT=5173` to select a fixed port in CI or a restricted runtime.
The test requires permission to bind a local server and a Chromium executable;
`PUPPETEER_EXECUTABLE_PATH` can override the default path.

## MVP boundaries

- Source files are never overwritten. Lucia keeps the imported bytes and edits
  a validated in-memory working stage with a bounded undo history.
- Retopology supports static triangulated meshes. Reduction is deterministic,
  but is not a production UV-aware remesher.
- Shading bake is a direct-light browser preview bake, not an offline path
  tracer or global-illumination bake.
- USDZ asset renames are applied to the newly exported package; the imported
  archive remains unchanged.
- Remote assistant requests contain a compact scene summary and tool results,
  never source USD or texture bytes.

## Stitch UDIM textures

Open the **Textures** inspector, then choose **Detect UDIM sets**. USDZ imports
retain embedded tiles. For loose USDA/USDC files, use **Add tile files** with a
relative directory prefix, or **Add asset folder** (folder paths include the
selected folder name). Conflicting paths are rejected before any assets change.

Check the texture shaders to process; every animated file opinion and shared
consumer of each checked shader is included. **Select all** and **Clear selection**
operate on sets with resolved tiles; unresolved sets show their errors and cannot
be checked. Preview stays disabled until a ready set is selected, and numeric
limits are validated before starting the worker. Detect sets again after a scene
edit, asset upload, undo, or redo to refresh the list. **Preview stitching** runs in a
worker and shows atlas thumbnails, dimensions, consumers, and a staged viewport.
**Apply preview** commits the USD and generated images as one undoable command.
**Discard preview** restores the working viewport. Scene, asset, or option changes
make a preview stale. Cancel stops the worker without applying its result.

Grid is the default and adds a UV transform. Dense is explicit and may rewrite
geometry and add atlas UV primvars/readers. Defaults are 100 tiles per layout,
an 8192-pixel atlas edge, 256 MiB working memory, crossing faces rejected,
2-pixel dense gutters, and subdivision level 2. Exceeding a limit fails without
dropping tiles. Resolution and encoded precision are preserved. Thumbnails are
bounded to 256 pixels and generated from the resident atlas pixels, including
HDR and EXR, without decoding the encoded atlas again. Floating-point thumbnails
use Reinhard tone mapping and sRGB encoding for display; atlas values are
unchanged. A thumbnail is omitted if its scratch space does not fit the working
budget. Source tiles remain in the project.

USDZ export packages applied atlases normally. For USDA exports, download the
atlases separately and preserve their displayed relative paths. Retained project
assets have a 256 MiB limit; the 128 MiB undo-history budget includes texture
snapshots. An edit too large to remain undoable is rejected and rolled back.
Unchanged texture snapshots share storage across edits; an in-place byte change
creates a separate snapshot, preserving previous undo states.
Independent export-time stitching and assistant tools are not part of this UI.

Scene replacement cancels active workers and waits for edit rollback. The latest
Open or New Project request supersedes earlier file reads. Editing, history,
export, and UDIM viewport previews share a session activity gate, preventing
overlapping native authoring and viewport conversion.

Run `npm run test:lucia:performance` from `web/js` to measure undo retention and
two 1024-pixel UDIM tiles. The Lucia browser smoke test also reports import and
viewport rebuild times for a 131072-triangle grid. These workloads assert
buffer sharing, transfer counts, and geometry counts rather than machine-specific
timing thresholds.

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

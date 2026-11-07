# lightui Vendor Import

Upstream: https://github.com/syoyo/lightui (local: `~/work/lightui`)

Vendored **subset** snapshot, not a git submodule. Imported for
`examples/lusdquicklook`, which needs a portable software-rendered UI without
ImGui/GLFW/OpenGL (unlike `examples/lusdview`).

## Snapshot

Upstream `main` @ `ace7db70518b71a7970d5374e77be85c6c03b865` (2026-07-23),
imported 2026-07-30.

lightusd-owned files:

- `CMakeLists.txt` — ours, replaces upstream's (see below)
- `README.lightusd.md` — this file
- `fonts/hack_regular_ttf.{c,h}` — pre-generated, see below

## lightusd-local patches (must survive a re-sync)

**Native window handle + external rendering (also in upstream lightui).**

`lui_window_native_handle()` / `lui_window_set_external_render()` in
`window.h`/`lightui.c`, plus `external_render`/`native_window`/`native_kind` in
`struct lui_window_s` and their Win32 wiring. Used by `examples/ld3d12view` to
create a D3D12 swapchain on the lightui `HWND` while keeping lightui's software
surface for the HUD. When the vendored copy is re-synced to an upstream that
has these, drop this note.

**File drag-and-drop (Win32; also in upstream lightui).**

`lui_window_set_file_drop()` / `lui_window_take_dropped_file()`, the
`LUI_EVENT_FILE_DROP` event, optional `window_set_file_drop` /
`window_take_dropped_file` platform ops, and the `WM_DROPFILES` handler in
`platform_win32.c`. Used by `examples/ld3d12view` to open files dropped on the
window. Other backends leave the ops NULL (drop is a no-op there).

**`src/platform/x11/platform_x11.c` — `x11_sync_configured_size()` must not block.**

Upstream loops on `XNextEvent` until it observes a `ConfigureNotify`. That
assumes a reparenting window manager, which always reconfigures a newly mapped
window. With **no window manager** — Xvfb, a bare X session, a kiosk — the
window is created at exactly the requested size and is never reconfigured, so
no `ConfigureNotify` is ever sent and `lui_window_create()` hangs forever,
before the application can draw its first frame.

The patch replaces the blocking loop with a non-blocking
`XCheckTypedWindowEvent` drain after an `XFlush`. Behaviour is unchanged when a
WM is present; without one it returns immediately. Adopting the server's
configured size is an optimization, not a correctness requirement — the window
already has the requested size, and a later reconfigure still reaches the app
as an ordinary resize event (`StructureNotifyMask` is selected).

Worth reporting upstream. Verified with the interactive smoke test under Xvfb
(`examples/lusdquicklook/tests/run-quicklook-gui-smoke.sh`), which hangs without
the patch.

**`src/platform/x11/platform_x11.c` — synthesize multi-click counts.**

`lui_event_t::mouse_button.clicks` is documented as "1 = single, 2 = double",
but the X11 backend hardcoded it to `1`. X11 reports every button press
independently and has no notion of a double-click, so unless the backend
tracks the run, no application on Linux can ever observe one — silently
breaking every double-click feature (in lusdquicklook, both descend-into-
directory in the file list and click-to-focus in the viewport).

The patch tracks the last press (button, time, position) and extends the run
when the next press is the same button within 400 ms and 4 px, matching the
conventional thresholds. Releases report the count of the press that opened
the run. The Win32 and Cocoa backends get this from the OS and are untouched.
Worth reporting upstream.

**`src/{statusbar,toolbar,propgrid,toast}.c` — optional font-aware text.**

Upstream draws each character in these four widgets as a 5x10 filled rectangle
with a 7px advance, so their text renders as rows of grey blocks. Widgets that
already carry a `lui_font_t *font` (`combo`, `tabs`, `label`, `menu`, `radio`,
`text_input`, `numentry`, `text_edit`) render real glyphs instead. An app that
mixes the two — as lusdquicklook does — gets legible combo labels next to an
illegible status bar.

The patch adds the same optional `lui_font_t *font` field to these four
widgets and takes the glyph path when it is set, following the existing
pattern in `src/combo.c`:

```c
#ifdef LUI_HAVE_FONTS
    if (w->font) { lui_canvas_draw_text(canvas, tx, ty, s, len, w->font, col); }
    else {
#endif
        /* existing 5x10 rect fallback, unchanged */
#ifdef LUI_HAVE_FONTS
    }
#endif
```

Behaviour is bit-identical when `font == NULL`, which is the default after
`lui_*_init()`, so upstream callers are unaffected. `propgrid.c` routes its
`pg_draw_text()` helper through a `pg_draw_text_f()` that takes the font; the
macro keeps the six call sites unchanged. Worth reporting upstream.

## What was vendored

Kept: `include/`, `lightvg/`, `lighttype/` (minus `font_freetype.c`),
`src/*.c` top-level widgets/core, `src/internal/`, `src/platform/{x11,win32,cocoa}/`,
`third_party/{wuffs-v0.4.c,stb_image_write.h}`, `fonts/Hack-Regular.ttf`.

Dropped: `src/mcp/` (LLM-debug HTTP server + socket deps), `src/markdown*`,
`src/html*`, `src/export_*`, `src/image_cmp.c`, `src/geomap/`, `src/svg/`,
`src/vg/backends/`, `lightvg/src/backends/` (blend2d/thorvg/skia),
`src/platform/{wayland,wasm}/`, `tui/`, `examples/`, `tests/`, `benchmarks/`,
`subprojects/`, `ref/`, `doc/`, meson/xmake build files, and all fonts except
Hack-Regular.

Headers for dropped widgets (e.g. `include/lightui/markdown.h`) are still
present since `include/lightui/lightui.h` includes them; the corresponding
`.c` files are absent, so referencing those widgets is a link error rather than
a compile error. Add the `.c` back if you need one.

## Our CMakeLists.txt

Upstream's top-level `CMakeLists.txt` is not usable via `add_subdirectory`: it
unconditionally builds ~25 example executables and does
`find_package(Python3 REQUIRED)` for font embedding. Ours builds a single static
`lightui` target folding in lightvg + lighttype, guarded by
`if(TARGET lightui) return() endif()`.

Notable: the X11 backend carries its own minimal Xlib ABI declarations
(`src/platform/x11/lui_x11.h`) and `dlopen`s `libX11` at runtime
(`lui_x11_loader.h`), so there is **no build-time or link-time X dependency** —
only `${CMAKE_DL_LIBS}`. That property is why lightui was chosen; preserve it.

## Embedded font

`fonts/hack_regular_ttf.{c,h}` are generated **offline** so the build needs no
Python. Regenerate after replacing the TTF:

```
python3 ~/work/lightui/scripts/embed_binary_resource.py \
    --input fonts/Hack-Regular.ttf --out-dir fonts \
    --base hack_regular_ttf --api lui_embedded_font --mode array
```

Consume with `ltt_font_create_from_memory(lui_embedded_font_data(), lui_embedded_font_size(), px)`.

## C linkage

lightui is C99. Include from C++ via the umbrella header; upstream headers
already guard with `#ifdef __cplusplus` / `extern "C"`.

## Licenses

`LICENSE` (lightui, upstream), `fonts/LICENSE.md` (Hack font),
`third_party/wuffs-v0.4.c` (Apache-2.0/MIT, header comment),
`third_party/stb_image_write.h` (public domain / MIT).

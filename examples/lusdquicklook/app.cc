// SPDX-License-Identifier: Apache-2.0
#include "app.hh"
#include "byte-budget.hh"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

extern "C" {
#include "hack_regular_ttf.h"
}

namespace lusdql {

namespace fs = std::filesystem;

namespace {

// Toolbar control widths. Wide enough for the longest label ("roughness",
// "auto") at the theme font size without measuring at layout time.
constexpr int kModeComboW = 104;
constexpr int kBackendComboW = 72;
// Status-bar section widths. The left section auto-stretches into the rest.
constexpr int kStatusBackendW = 320;
constexpr int kStatusMemW = 260;
// Nominal frame time for toast fade. The loop is event-driven, so this is a
// pacing constant, not a measurement: a toast lives ~4s of frames either way.
constexpr float kFrameDt = 1.0f / 60.0f;

std::string ToLower(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

bool IsImagePathInternal(const std::string& path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) return false;
  const size_t slash = path.find_last_of("/\\");
  if (slash != std::string::npos && dot < slash) return false;
  const std::string ext = ToLower(path.substr(dot));
  return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" ||
         ext == ".gif" || ext == ".tga";
}

bool IsGlBudgetError(const std::string& error) {
  return error.find("not enough GPU memory") != std::string::npos;
}

// Bounded shallow search for an image to use as a folder tile's preview
// thumbnail: the folder's own files, then (one level only) its immediate
// subfolders. Depth-limited so a folder with many subfolders doesn't turn
// into an unbounded recursive scan; returns "" if nothing turns up.
std::string FindRepresentativeImage(const fs::path& dir, int max_depth) {
  std::error_code ec;
  fs::directory_iterator it(
      dir, fs::directory_options::skip_permission_denied, ec);
  if (ec) return std::string();

  std::vector<fs::path> subdirs;
  for (const auto& de : it) {
    std::error_code ec2;
    if (de.is_regular_file(ec2) && !ec2) {
      const std::string p = de.path().lexically_normal().string();
      if (IsImagePathInternal(p)) return p;
    } else if (de.is_directory(ec2) && !ec2) {
      subdirs.push_back(de.path());
    }
  }
  if (max_depth > 1) {
    std::sort(subdirs.begin(), subdirs.end());
    for (const auto& sd : subdirs) {
      const std::string found = FindRepresentativeImage(sd, max_depth - 1);
      if (!found.empty()) return found;
    }
  }
  return std::string();
}

#if defined(_WIN32)
// The custom (non-FreeType) glyph backend only understands single-font
// sfnt files, not TrueType Collections — so this list is restricted to the
// standalone .ttf/.otf CJK fonts Windows actually ships (most East Asian
// system fonts, e.g. Meiryo, Yu Gothic, MS Gothic, are .ttc and unusable
// here). First match wins; missing files are silently skipped.
const char* const kWindowsCjkFontCandidates[] = {
    "NotoSansJP-VF.ttf",
    "NotoSansSC-VF.ttf",
    "NotoSansTC-VF.ttf",
    "NotoSansKR-VF.ttf",
    "NotoSerifJP-VF.ttf",
    "yumin.ttf",
    "malgun.ttf",
};

// Best-effort path to a UTF-8/CJK-capable system font. Empty when none of
// the candidates are present (falls back to the embedded Latin-only font).
std::string FindWindowsCjkFontPath() {
  const char* sysroot = std::getenv("SystemRoot");
  const fs::path fonts_dir = fs::path(sysroot ? sysroot : "C:\\Windows") / "Fonts";
  std::error_code ec;
  for (const char* name : kWindowsCjkFontCandidates) {
    const fs::path candidate = fonts_dir / name;
    if (fs::exists(candidate, ec) && !ec) {
      return candidate.string();
    }
  }
  return std::string();
}
#endif  // _WIN32

}  // namespace

App::App(const Options& opts)
    : opts_(opts), theme_(DefaultTheme()), view_mode_(ViewMode::UsdPreview) {}

App::~App() {
#if defined(LUSDQUICKLOOK_HAVE_MCP)
  if (mcp_) {
    mcp_->stop();
    mcp_.reset();
  }
#endif
  StopImageWorkers();
  if (font_) {
    lui_font_destroy(font_);
    font_ = nullptr;
  }
  if (window_) {
    lui_window_destroy(window_);
    window_ = nullptr;
  }
  if (platform_up_) {
    lui_shutdown();
    platform_up_ = false;
  }
}

bool App::Init() {
  // Fonts, canvas and surfaces are all platform-independent; only windows need
  // the platform layer, and lui_init() opens the display. Skipping it when
  // headless is what lets --screenshot work over ssh and in CI with no X.
  headless_ = !opts_.screenshot.empty();

  // Seed the live UI state from the CLI so the toolbar and the flags are the
  // same setting, not two.
  shading_mode_ = opts_.shading_mode;
  ibl_enabled_ = opts_.ibl;
  shadows_enabled_ = opts_.shadows;
  desired_backend_ = opts_.backend;

  if (!headless_) {
    if (!lui_init()) {
      err_ =
          "lui_init() failed (no display? use --screenshot for headless "
          "rendering)";
      return false;
    }
    platform_up_ = true;
  }

  // Prefer a UTF-8/CJK-capable Windows system font so non-Latin prim names,
  // asset paths, and metadata render as glyphs instead of tofu boxes; the
  // embedded Hack font is Latin-only. Falls back to it wherever no such
  // system font is found (non-Windows, or a CJK-less install).
#if defined(_WIN32)
  const std::string cjk_font_path = FindWindowsCjkFontPath();
  if (!cjk_font_path.empty()) {
    font_ = lui_font_create(cjk_font_path.c_str(), theme_.font_px);
    if (font_ && opts_.verbose) {
      std::fprintf(stderr, "[lusdquicklook] font: %s\n", cjk_font_path.c_str());
    }
  }
#endif
  if (!font_) {
    font_ = lui_font_create_from_memory(lui_embedded_font_data(),
                                        lui_embedded_font_size(),
                                        theme_.font_px);
  }
  if (!font_) {
    err_ = "failed to create the embedded font";
    return false;
  }

  // Install the process cap first: everything downstream (browser projections,
  // loader, renderer) sizes itself against the *clamped* value, which may be
  // lower than requested on a small machine.
  budget_ = PreviewBudget::Install(opts_.max_mem_bytes);
  if (budget_.total != opts_.max_mem_bytes && opts_.verbose) {
    std::fprintf(stderr,
                 "[lusdquicklook] memory budget clamped to %s "
                 "(requested %s, limited by available system memory)\n",
                 FormatBytes(budget_.total).c_str(),
                 FormatBytes(opts_.max_mem_bytes).c_str());
  }

  browser_.SetRecursive(opts_.recursive);
  browser_.SetMemoryBudget(budget_.total);

  std::string berr;
  if (!browser_.Open(opts_.path, &berr)) {
    err_ = berr;
    return false;
  }
  status_left_ = browser_.dir();

  if (const FileEntry* sel = browser_.SelectedEntry()) {
    // Headless runs the load synchronously in RunHeadless(); starting a worker
    // here would just race it.
    if (headless_) {
      viewport_message_ = sel->name;
      if (sel->over_budget) {
        viewport_message_ = sel->name + " - too large for quick look (projected " +
                            FormatBytes(sel->projected_bytes) + ", budget " +
                            FormatBytes(budget_.total) + ")";
        viewport_message_is_error_ = true;
      }
    } else {
      PreviewFile(*sel);
    }
  } else {
    viewport_message_ = "no USD files in this folder";
  }
  if (view_mode_ == ViewMode::ImageBrowser) {
    BuildImageItems();
  }

  if (headless_) {
    surf_w_ = opts_.width;
    surf_h_ = opts_.height;
    return true;
  }

  window_ = lui_window_create("lusdquicklook", opts_.width, opts_.height,
                              LUI_WINDOW_RESIZABLE);
  if (!window_) {
    err_ =
        "failed to create a window (no display? try --screenshot for headless "
        "rendering)";
    return false;
  }
  lui_window_get_physical_size(window_, &surf_w_, &surf_h_);
  // Widgets exist only in the windowed path: headless has nothing to interact
  // with, and a toast there would make the screenshot time-dependent.
  InitWidgets();
  lui_window_show(window_);

#if defined(LUSDQUICKLOOK_HAVE_MCP)
  if (opts_.mcp_stdio || opts_.mcp_http_port != 0) {
    mcp_ = std::make_unique<MCPServer>(this);
    if (opts_.mcp_stdio) mcp_->startStdio();
    if (opts_.mcp_http_port != 0 &&
        !mcp_->startHttp(opts_.mcp_http_port)) {
      err_ = "failed to start MCP HTTP server on port " +
             std::to_string(opts_.mcp_http_port);
      return false;
    }
  }
#endif
  return true;
}

bool App::Busy() const {
  // While a load is in flight, or the image has not converged, the loop polls
  // instead of blocking so results appear as they arrive rather than on the
  // next mouse move.
  if (loader_.running() || !scene_complete_) return true;
  if (view_mode_ == ViewMode::ImageBrowser && HasImageBrowserWork()) return true;
  // A visible toast animates, so the loop must keep ticking until it expires —
  // and must stop as soon as it does, or the app never goes idle again.
  if (ToastActive()) return true;
  // A settling camera is work: the loop has to keep drawing until it arrives.
  // ApproachToward snaps and clears the flag, so this always terminates.
  if (camera_animating_) return true;
#if defined(LUSDQUICKLOOK_HAVE_MCP)
  // MCP tool calls are queued by transport threads and need the main loop to
  // keep draining even when the preview is idle. The short sleep in the busy
  // path keeps this polling cheap when no request is pending.
  if (mcp_) return true;
#endif
  return renderer_ && !scene_.meshes.empty() && !render_status_.converged;
}

int App::Run() {
  // Paint BEFORE blocking for input, not after. The obvious ordering (wait for
  // an event, then draw) never paints the first frame: at startup there is
  // nothing pending, so the loop blocks in wait_event and the window stays
  // blank until the user happens to move the mouse. Headless --screenshot runs
  // cannot catch that, since they never touch the event loop.
  while (!quit_) {
#if defined(LUSDQUICKLOOK_HAVE_MCP)
    if (mcp_) mcp_->drain();
    if (mcp_ && opts_.mcp_http_port == 0 && mcp_->stdioClosed()) {
      // A stdio client disappearing should not leave a hidden GUI process
      // behind. With HTTP enabled, the HTTP transport owns the process lifetime.
      quit_ = true;
    }
#endif
    if (DrainLoadEvents()) needs_redraw_ = true;
    if (DrainImageThumbnailEvents()) needs_redraw_ = true;

    // Progressive refinement: keep repainting until the image converges.
    if (renderer_ && !scene_.meshes.empty() && !render_status_.converged) {
      needs_redraw_ = true;
    }

    if (needs_redraw_) {
      lvg_surface_t* surf = lui_window_get_surface(window_);
      if (surf) {
        surf_w_ = surf->width;
        surf_h_ = surf->height;
        DrawFrame(surf);
        lui_window_present(window_);
      }
      needs_redraw_ = false;
    }

    lui_event_t ev;
    if (Busy()) {
      bool got_event = false;
      while (lui_window_poll_event(window_, &ev)) {
        got_event = true;
        if (!HandleEvent(ev)) {
          quit_ = true;
          break;
        }
      }
      // Nothing to do this tick: yield rather than spin. A previewer that pins
      // a core while it waits on a worker would defeat the point.
        if (!got_event && !needs_redraw_ && !StreamHasWork() &&
            !HasImageBrowserWork()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    } else {
      // Idle and converged: block until something actually happens.
      if (!lui_window_wait_event(window_, &ev)) break;
      if (!HandleEvent(ev)) break;
      // Coalesce whatever else arrived with it (mouse-move floods especially).
      while (lui_window_poll_event(window_, &ev)) {
        if (!HandleEvent(ev)) {
          quit_ = true;
          break;
        }
      }
    }
  }
  return 0;
}

int App::RunHeadless() {
  lvg_surface_t* surf = lvg_surface_create(surf_w_, surf_h_);
  if (!surf) {
    err_ = "failed to allocate the offscreen surface";
    return 1;
  }

  // Run the load on this thread rather than a worker: headless output must be
  // deterministic, not a race between the loader and a frame counter.
  if (const FileEntry* sel = browser_.SelectedEntry()) {
    if (!sel->is_dir && !sel->over_budget) {
      auto ctrl = std::make_shared<LoadControl>();
      ctrl->Reset();
      ctrl->max_stage_bytes = budget_.stage;
      ctrl->max_geometry_bytes = budget_.geometry;
      ctrl->max_texture_bytes = budget_.textures;
      // Unbounded queue: nothing is draining concurrently, so a byte bound
      // here would deadlock the single-threaded run.
      auto stream = std::make_shared<LoadStream>(UINT64_MAX);
      RunLoad(sel->path, opts_, budget_, ctrl, stream);

      LoadEvent ev;
      while (stream->TryPop(&ev)) {
        ApplyLoadEvent(std::move(ev));
        ev = LoadEvent{};
      }
      UpdateStatus();
    }
  }

  for (int i = 0; i < opts_.frames; i++) {
    DrawFrame(surf);
  }

  const int rc = lvg_surface_save_png(surf, opts_.screenshot.c_str());
  lvg_surface_destroy(surf);
  if (rc != 0) {
    err_ = "failed to write " + opts_.screenshot;
    return 1;
  }
  if (opts_.verbose) {
    std::fprintf(stderr,
                 "[lusdquicklook] wrote %s (%dx%d) spp=%d/%d tiles=%d/%d "
                 "converged=%d\n",
                 opts_.screenshot.c_str(), surf_w_, surf_h_,
                 render_status_.samples_done, render_status_.samples_target,
                 render_status_.tiles_done, render_status_.tiles_total,
                 render_status_.converged ? 1 : 0);
  }
  return 0;
}

bool App::HandleEvent(const lui_event_t& ev) {
  // lightui widgets get first refusal on pointer events. An open dropdown must
  // swallow clicks that would otherwise land on the toolbar or the viewport
  // behind it, so this runs before any of the app's own hit-testing.
  if (widgets_ready_ && (ev.type == LUI_EVENT_MOUSE_DOWN ||
                         ev.type == LUI_EVENT_MOUSE_UP ||
                         ev.type == LUI_EVENT_MOUSE_MOVE)) {
    lui_combo_t* combos[2] = {&mode_combo_, &backend_combo_};
    for (lui_combo_t* cb : combos) {
      const bool was_open = cb->open;
      if (cb->widget.on_event && cb->widget.on_event(&cb->widget, &ev)) {
        needs_redraw_ = true;
        return true;
      }
      // Opening or closing changes what is on screen even when the widget did
      // not claim the event.
      if (cb->open != was_open) needs_redraw_ = true;
    }
    // A click anywhere else dismisses an open dropdown rather than leaving it
    // hanging over the viewport.
    if (ev.type == LUI_EVENT_MOUSE_DOWN) {
      for (lui_combo_t* cb : combos) {
        if (cb->open) {
          cb->open = false;
          needs_redraw_ = true;
          return true;
        }
      }
    }
  }

  switch (ev.type) {
    case LUI_EVENT_QUIT:
      return false;

    case LUI_EVENT_WINDOW_EXPOSE:
      needs_redraw_ = true;
      break;

    case LUI_EVENT_WINDOW_RESIZE:
      needs_redraw_ = true;
      break;

    case LUI_EVENT_KEY_DOWN: {
      if (ev.data.key.key == LUI_KEY_ESCAPE) return false;
      if ((ev.data.key.mods & LUI_MOD_CTRL) && ev.data.key.key == 'q') {
        return false;
      }
        const int rows = VisibleRows();
        switch (ev.data.key.key) {
        case 't':
          ToggleViewMode();
          needs_redraw_ = true;
          break;
        case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': {
          // Shading mode by number, in the combo's order.
          const int idx = ev.data.key.key - '1';
          if (idx < kShadingModeCount) {
            shading_mode_ = static_cast<ShadingMode>(idx);
            ApplyRenderSettings();
            needs_redraw_ = true;
          }
          break;
        }
        case 'g':
          // Cycle the backend preference. EnsureRenderer picks the change up on
          // the next frame, which is also where the switch actually happens —
          // creating a context from inside an event handler would tear down the
          // renderer while DrawViewport still holds its pixels.
          CycleBackend();
          needs_redraw_ = true;
          break;
        case LUI_KEY_UP:
          if (view_mode_ == ViewMode::ImageBrowser) {
            if (MoveImageSelection(-image_columns_)) {
              needs_redraw_ = true;
            }
          } else if (browser_.MoveSelection(-1)) {
            browser_.EnsureSelectionVisible(rows);
            if (const FileEntry* s = browser_.SelectedEntry()) PreviewFile(*s);
            needs_redraw_ = true;
          }
          break;
        case LUI_KEY_DOWN:
          if (view_mode_ == ViewMode::ImageBrowser) {
            if (MoveImageSelection(image_columns_)) {
              needs_redraw_ = true;
            }
          } else if (browser_.MoveSelection(1)) {
            browser_.EnsureSelectionVisible(rows);
            if (const FileEntry* s = browser_.SelectedEntry()) PreviewFile(*s);
            needs_redraw_ = true;
          }
          break;
        case LUI_KEY_PAGE_UP:
          if (view_mode_ == ViewMode::ImageBrowser) {
            if (MoveImageSelection(-std::max(1, rows - 1) * image_columns_)) {
              needs_redraw_ = true;
            }
          } else if (browser_.MoveSelection(-std::max(1, rows - 1))) {
            browser_.EnsureSelectionVisible(rows);
            if (const FileEntry* s = browser_.SelectedEntry()) PreviewFile(*s);
            needs_redraw_ = true;
          }
          break;
        case LUI_KEY_PAGE_DOWN:
          if (view_mode_ == ViewMode::ImageBrowser) {
            if (MoveImageSelection(std::max(1, rows - 1) * image_columns_)) {
              needs_redraw_ = true;
            }
          } else if (browser_.MoveSelection(std::max(1, rows - 1))) {
            browser_.EnsureSelectionVisible(rows);
            if (const FileEntry* s = browser_.SelectedEntry()) PreviewFile(*s);
            needs_redraw_ = true;
          }
          break;
        case LUI_KEY_HOME:
          if (view_mode_ == ViewMode::ImageBrowser) {
            bool selected = false;
            {
              std::lock_guard<std::mutex> lock(image_mu_);
              if (!image_items_.empty() && selected_image_ != 0) {
                selected_image_ = 0;
                selected = true;
              }
            }
            if (selected) {
              needs_redraw_ = true;
            }
          } else if (browser_.Select(0)) {
            browser_.EnsureSelectionVisible(rows);
            if (const FileEntry* s = browser_.SelectedEntry()) PreviewFile(*s);
            needs_redraw_ = true;
          }
          break;
        case LUI_KEY_END:
          if (view_mode_ == ViewMode::ImageBrowser) {
            int last = -1;
            {
              std::lock_guard<std::mutex> lock(image_mu_);
              last = static_cast<int>(image_items_.size()) - 1;
            }
            if (selected_image_ != last && last >= 0) {
              selected_image_ = last;
              needs_redraw_ = true;
            }
          } else if (browser_.Select(browser_.RowCount() - 1)) {
            browser_.EnsureSelectionVisible(rows);
            if (const FileEntry* s = browser_.SelectedEntry()) PreviewFile(*s);
            needs_redraw_ = true;
          }
          break;
        case LUI_KEY_RETURN:
          if (view_mode_ == ViewMode::ImageBrowser) {
            std::string name;
            std::string dir_path;
            bool is_dir = false;
            {
              std::lock_guard<std::mutex> lock(image_mu_);
              if (selected_image_ >= 0 &&
                  selected_image_ < static_cast<int>(image_items_.size())) {
                const ImageItem& sel =
                    image_items_[static_cast<size_t>(selected_image_)];
                name = sel.name;
                dir_path = sel.nav_path;
                is_dir = sel.is_dir;
              }
            }
            if (is_dir) {
              EnterImageFolder(dir_path);
            } else if (!name.empty()) {
              viewport_message_ = name;
            }
          } else {
            ActivateSelection();
            browser_.EnsureSelectionVisible(rows);
          }
          break;
        default:
          if (ev.data.key.key == 'r' && !(ev.data.key.mods & LUI_MOD_CTRL)) {
            RefreshFolder();
          } else if (ev.data.key.key == 'f' &&
                     (ev.data.key.mods & LUI_MOD_SHIFT) && have_pick_ &&
                     pick_mesh_ < scene_.meshes.size()) {
            // Shift+F frames just the picked mesh.
            const Layout fl = ComputeLayout(theme_, surf_w_, surf_h_,
                                            left_pane_w_);
            const float aspect =
                fl.viewport.height > 0
                    ? float(fl.viewport.width) / float(fl.viewport.height)
                    : 1.0f;
            camera_goal_.FrameBounds(scene_.meshes[pick_mesh_].bounds, aspect);
            camera_user_controlled_ = true;
            needs_redraw_ = true;
          } else if (ev.data.key.key == 'f') {
            // Give the camera back to auto-framing.
            camera_user_controlled_ = false;
            camera_framed_ = false;
            needs_redraw_ = true;
          } else if (ev.data.key.key == '.' && have_pick_) {
            // Re-centre on the last pick without re-picking.
            camera_goal_.SetTarget(pick_point_);
            camera_user_controlled_ = true;
            needs_redraw_ = true;
          } else if ((ev.data.key.mods == 0) &&
                     (ev.data.key.key == '[' || ev.data.key.key == '{')) {
            const int next = std::max(kImageMinColumns, image_columns_ - 1);
            if (next != image_columns_) {
              image_columns_ = next;
              if (view_mode_ == ViewMode::ImageBrowser) needs_redraw_ = true;
            }
          } else if ((ev.data.key.mods == 0) &&
                     (ev.data.key.key == ']' || ev.data.key.key == '}')) {
            const int next = std::min(kImageMaxColumns, image_columns_ + 1);
            if (next != image_columns_) {
              image_columns_ = next;
              if (view_mode_ == ViewMode::ImageBrowser) needs_redraw_ = true;
            }
          }
          break;
      }
      break;
    }

    case LUI_EVENT_SCROLL: {
      const Layout l = ComputeLayout(theme_, surf_w_, surf_h_, left_pane_w_);
      const int x = ev.data.scroll.x;
      const int y = ev.data.scroll.y;
      if (lvg_rect_contains_point(&l.list, x, y)) {
        // delta_y > 0 scrolls down.
        const int rows_delta =
            static_cast<int>(ev.data.scroll.delta_y > 0.0f ? 3 : -3);
        if (ev.data.scroll.delta_y != 0.0f &&
            browser_.ScrollBy(rows_delta, VisibleRows())) {
          needs_redraw_ = true;
        }
      } else if (lvg_rect_contains_point(&l.viewport, x, y) &&
                 ev.data.scroll.delta_y != 0.0f) {
        if (view_mode_ == ViewMode::ImageBrowser) {
          if (ScrollImageByMouseWheel(
                  static_cast<int>(ev.data.scroll.delta_y > 0.0f ? 1 : -1))) {
            needs_redraw_ = true;
          }
        } else {
          camera_user_controlled_ = true;
          camera_goal_.Dolly(ev.data.scroll.delta_y > 0.0f ? 1.12f
                                                          : 1.0f / 1.12f);
          needs_redraw_ = true;
        }
      }
      break;
    }

    case LUI_EVENT_MOUSE_DOWN: {
      const Layout l = ComputeLayout(theme_, surf_w_, surf_h_, left_pane_w_);
      const int x = ev.data.mouse_button.x;
      const int y = ev.data.mouse_button.y;
      const lvg_rect_t reset_btn = ResetButtonRect(l.toolbar);
      const lvg_rect_t refresh_btn = RefreshButtonRect(l.toolbar);
      if (lvg_rect_contains_point(&reset_btn, x, y) &&
          ev.data.mouse_button.button == LUI_MOUSE_LEFT) {
        ResetShadingAndViewport();
        needs_redraw_ = true;
        break;
      } else if (lvg_rect_contains_point(&refresh_btn, x, y) &&
                 ev.data.mouse_button.button == LUI_MOUSE_LEFT) {
        RefreshFolder();
        needs_redraw_ = true;
        break;
      }
      if (lvg_rect_contains_point(&l.splitter, x, y)) {
        dragging_splitter_ = true;
        drag_origin_x_ = x;
        drag_origin_w_ = left_pane_w_;
      } else if (lvg_rect_contains_point(&l.viewport, x, y)) {
        if (view_mode_ == ViewMode::ImageBrowser &&
            ev.data.mouse_button.button == LUI_MOUSE_LEFT) {
          const ImageGridGeometry geo =
              ComputeImageGridGeometry(l.viewport.width, l.viewport.height);
          if (geo.cell_w > 0 && geo.cell_h > 0) {
            const int gx = x - l.viewport.x;
            const int gy = y - l.viewport.y;
            const int gx_cell = gx / std::max(1, geo.cell_w);
            const int gy_cell = gy / std::max(1, geo.cell_h) + image_scroll_;
            const int idx = gy_cell * geo.columns + gx_cell;
            std::string clicked_name;
            std::string clicked_path;
            bool clicked_is_dir = false;
            if (gx >= 0 && gy >= 0 && idx >= 0 &&
                idx < static_cast<int>(image_items_.size())) {
              {
                std::lock_guard<std::mutex> lock(image_mu_);
                if (idx >= 0 &&
                    idx < static_cast<int>(image_items_.size())) {
                  selected_image_ = idx;
                  const ImageItem& clicked =
                      image_items_[static_cast<size_t>(idx)];
                  clicked_name = clicked.name;
                  clicked_path = clicked.nav_path;
                  clicked_is_dir = clicked.is_dir;
                }
              }
              needs_redraw_ = true;
              if (ev.data.mouse_button.clicks >= 2 && clicked_is_dir) {
                EnterImageFolder(clicked_path);
              } else if (ev.data.mouse_button.clicks >= 2 &&
                         !clicked_name.empty()) {
                viewport_message_ = clicked_name;
              }
            }
          }
          break;
        }

        // Double-click re-centres the orbit on whatever is under the cursor,
        // which is what makes anything bigger than one object navigable.
        if (ev.data.mouse_button.button == LUI_MOUSE_LEFT &&
            ev.data.mouse_button.clicks >= 2) {
          if (PickAt(l.viewport, x, y)) {
            needs_redraw_ = true;
            break;
          }
        }

        // Left drag orbits, middle/right drag pans.
        drag_mode_ = (ev.data.mouse_button.button == LUI_MOUSE_LEFT)
                         ? DragMode::Orbit
                         : DragMode::Pan;
        drag_last_x_ = x;
        drag_last_y_ = y;
      } else if (lvg_rect_contains_point(&l.list, x, y)) {
        const int row = browser_.scroll() + (y - l.list.y) / theme_.row_h;
        if (row >= 0 && row < browser_.RowCount()) {
          const bool changed = browser_.Select(row);
          // A double-click on a directory descends; on a file it re-previews,
          // which is harmless.
          if (ev.data.mouse_button.clicks >= 2) {
            ActivateSelection();
          } else if (changed && view_mode_ == ViewMode::UsdPreview) {
            if (const FileEntry* s = browser_.SelectedEntry()) PreviewFile(*s);
          }
          needs_redraw_ = true;
        }
      }
      break;
    }

    case LUI_EVENT_MOUSE_UP:
      dragging_splitter_ = false;
      drag_mode_ = DragMode::None;
      break;

    case LUI_EVENT_MOUSE_MOVE:
      if (dragging_splitter_) {
        const int delta = ev.data.mouse_move.x - drag_origin_x_;
        const int w = ClampPaneWidth(drag_origin_w_ + delta, surf_w_);
        if (w != left_pane_w_) {
          left_pane_w_ = w;
          needs_redraw_ = true;
        }
      } else if (drag_mode_ != DragMode::None) {
        const int dx = ev.data.mouse_move.x - drag_last_x_;
        const int dy = ev.data.mouse_move.y - drag_last_y_;
        drag_last_x_ = ev.data.mouse_move.x;
        drag_last_y_ = ev.data.mouse_move.y;
        if (dx != 0 || dy != 0) {
          camera_user_controlled_ = true;
          const Layout l = ComputeLayout(theme_, surf_w_, surf_h_, left_pane_w_);
          if (drag_mode_ == DragMode::Orbit) {
            // ~half a turn across the viewport width.
            const float scale = 3.14159265f / std::max(1, l.viewport.width);
            camera_goal_.Orbit(-float(dx) * scale, float(dy) * scale);
          } else {
            camera_goal_.Pan(float(dx), float(dy), l.viewport.height);
          }
          needs_redraw_ = true;
        }
      }
      break;

    default:
      break;
  }
  return true;
}

void App::DrawFrame(lvg_surface_t* surf) {
  lvg_canvas_t canvas;
  lvg_canvas_init(&canvas, surf);

  const Layout l = ComputeLayout(theme_, surf->width, surf->height,
                                 left_pane_w_);

  StepCameraMotion(kFrameDt);

  if (widgets_ready_) {
    PlaceWidgets(l);
    // Toasts fade; advance them before drawing so an expired one disappears
    // this frame and Busy() can go false again.
    if (toast_.widget.animate) {
      toast_.widget.animate(&toast_.widget, kFrameDt);
    }
  }

  lvg_canvas_clear(&canvas, theme_.bg);
  DrawToolbar(&canvas, l.toolbar);
  DrawListPane(&canvas, l.list);
  DrawSplitter(&canvas, l.splitter);
  if (view_mode_ == ViewMode::ImageBrowser) {
    DrawImageBrowser(&canvas, l.viewport);
  } else {
    DrawViewport(&canvas, l.viewport);
  }
  // Refresh the status text only now: DrawViewport is what creates or switches
  // the renderer, so reading the live backend any earlier reports the previous
  // frame's answer -- and once the image converges there is no next frame to
  // correct it, leaving a GL session permanently labelled "cpu".
  if (widgets_ready_) SyncWidgetState();
  DrawStatusBar(&canvas, l.statusbar);

  // Overlays last, so an open dropdown or a toast is never clipped by the
  // panels drawn after the widget that owns it.
  if (widgets_ready_) {
    lui_combo_draw_dropdown(&mode_combo_, &canvas);
    lui_combo_draw_dropdown(&backend_combo_, &canvas);
    if (toast_.toast_count > 0 && toast_.widget.draw) {
      toast_.widget.draw(&toast_.widget, &canvas);
    }
  }

  lvg_canvas_flush(&canvas);
}

void App::OnModeComboChanged(int index, const char* /*item*/, void* user) {
  App* self = static_cast<App*>(user);
  if (index < 0 || index >= kShadingModeCount) return;
  self->shading_mode_ = static_cast<ShadingMode>(index);
  self->ApplyRenderSettings();
  self->needs_redraw_ = true;
}

void App::OnBackendComboChanged(int index, const char* /*item*/, void* user) {
  App* self = static_cast<App*>(user);
  const BackendChoice choices[3] = {BackendChoice::Auto, BackendChoice::Cpu,
                                    BackendChoice::Gl};
  if (index < 0 || index > 2) return;
  self->desired_backend_ = choices[index];
  if (self->desired_backend_ == BackendChoice::Gl) {
    // An explicit retry clears a previous failure, same as the 'g' hotkey.
    self->gl_disabled_ = false;
    self->gl_budget_blocked_ = false;
    self->gl_error_.clear();
  }
  // EnsureRenderer performs the actual switch on the next frame; tearing the
  // renderer down from inside an event handler would free pixels the current
  // frame may still be holding.
  self->needs_redraw_ = true;
}

void App::InitWidgets() {
  if (widgets_ready_) return;

  lui_combo_init(&mode_combo_);
  mode_combo_.font = font_;
  for (int i = 0; i < kShadingModeCount; i++) {
    lui_combo_add_item(&mode_combo_,
                       ShadingModeName(static_cast<ShadingMode>(i)));
  }
  lui_combo_set_selected(&mode_combo_, static_cast<int>(shading_mode_));
  mode_combo_.on_change = &App::OnModeComboChanged;
  mode_combo_.on_change_user = this;

  lui_combo_init(&backend_combo_);
  backend_combo_.font = font_;
  lui_combo_add_item(&backend_combo_, "auto");
  lui_combo_add_item(&backend_combo_, "cpu");
  lui_combo_add_item(&backend_combo_, "gl");
  lui_combo_set_selected(
      &backend_combo_, desired_backend_ == BackendChoice::Auto ? 0
                       : desired_backend_ == BackendChoice::Cpu ? 1
                                                                : 2);
  backend_combo_.on_change = &App::OnBackendComboChanged;
  backend_combo_.on_change_user = this;

  lui_statusbar_init(&statusbar_);
  statusbar_.font = font_;
  statusbar_.bg_color = theme_.panel;
  statusbar_.text_color = theme_.text_dim;
  statusbar_.border_color = theme_.border;
  statusbar_.separator_color = theme_.border;
  // Left section auto-stretches; the rest are sized by content at draw time.
  lui_statusbar_add_section(&statusbar_, "ready", 0);
  lui_statusbar_add_section(&statusbar_, "", kStatusBackendW);
  lui_statusbar_add_section(&statusbar_, "", kStatusMemW);

  lui_toast_init(&toast_);
  toast_.font = font_;

  widgets_ready_ = true;

  // A backend failure can happen on the first painted frame, after widgets
  // exist. Surface it here if the user explicitly asked for GL.
  if ((gl_disabled_ || gl_budget_blocked_) &&
      desired_backend_ == BackendChoice::Gl) {
    Notify((gl_budget_blocked_ ? "GL budget exceeded (" : "GL unavailable (") +
               gl_error_ + ")",
           LUI_TOAST_WARNING);
  }
}

void App::PlaceWidgets(const Layout& l) {
  // A parentless widget's `computed` rect is its absolute rect.
  const int combo_h = std::min(l.toolbar.height - 4, 24);
  const int y = l.toolbar.y + (l.toolbar.height - combo_h) / 2;

  const int mode_w = kModeComboW;
  const int backend_w = kBackendComboW;
  // Right-aligned, inboard of the Reset/Refresh buttons.
  const lvg_rect_t reset = ResetButtonRect(l.toolbar);
  const int right_edge =
      (reset.width > 0) ? reset.x - theme_.pad
                        : l.toolbar.x + l.toolbar.width - theme_.pad;

  backend_combo_.widget.computed =
      lvg_rect_make(right_edge - backend_w, y, backend_w, combo_h);
  mode_combo_.widget.computed = lvg_rect_make(
      right_edge - backend_w - theme_.pad - mode_w, y, mode_w, combo_h);

  statusbar_.widget.computed = l.statusbar;
  toast_.widget.computed = l.viewport;
}

void App::SyncWidgetState() {
  // The widgets mirror app state rather than owning it, so a hotkey and a
  // click land in the same place.
  lui_combo_set_selected(&mode_combo_, static_cast<int>(shading_mode_));
  lui_combo_set_selected(
      &backend_combo_, desired_backend_ == BackendChoice::Auto ? 0
                       : desired_backend_ == BackendChoice::Cpu ? 1
                                                                : 2);

  lui_statusbar_set_text(&statusbar_, 0,
                         status_right_.empty() ? "ready"
                                               : status_right_.c_str());
  lui_statusbar_set_text(&statusbar_, 1, BackendStatusText().c_str());
  const std::string mem = "mem " + budget_.FormatUsage() + "  rss " +
                          FormatBytes(MemBudget::ProcessRSS());
  lui_statusbar_set_text(&statusbar_, 2, mem.c_str());
}

bool App::ToastActive() const {
  return widgets_ready_ && toast_.toast_count > 0;
}

void App::Notify(const std::string& message, lui_toast_type_t type) {
  // Headless has no one to show a toast to, and drawing one would make the
  // screenshot depend on wall-clock time.
  if (headless_ || !widgets_ready_) return;
  lui_toast_show(&toast_, message.c_str(), type, 4.0f);
  needs_redraw_ = true;
}

void App::DrawToolbar(lvg_canvas_t* c, const lvg_rect_t& r) {
  FillRect(c, r, theme_.panel);
  lvg_canvas_fill_rect(c, r.x, r.y + r.height - 1, r.width, 1, theme_.border);

  const lvg_rect_t refresh_button = RefreshButtonRect(r);
  const lvg_rect_t reset_button = ResetButtonRect(r);
  const int baseline = r.y + (r.height + lui_font_ascent(font_)) / 2 - 2;
  if (refresh_button.width > 0 && refresh_button.height > 0) {
    FillRect(c, refresh_button, theme_.selection);
    StrokeRect(c, refresh_button, theme_.text_dim);
    DrawTextCentered(c, font_, refresh_button, "Refresh", theme_.text);
  }
  if (reset_button.width > 0 && reset_button.height > 0) {
    FillRect(c, reset_button, theme_.selection);
    StrokeRect(c, reset_button, theme_.text_dim);
    DrawTextCentered(c, font_, reset_button, "Reset", theme_.text);
  }

  // The two combos are real lightui widgets; they draw their closed state here
  // and their dropdown later, as an overlay, so it is not clipped by whatever
  // is drawn after the toolbar.
  if (widgets_ready_) {
    if (mode_combo_.widget.draw) mode_combo_.widget.draw(&mode_combo_.widget, c);
    if (backend_combo_.widget.draw) {
      backend_combo_.widget.draw(&backend_combo_.widget, c);
    }
  }

  int left_max = r.width - 2 * theme_.pad;
  if (reset_button.width > 0) {
    left_max = std::max(0, reset_button.x - (r.x + theme_.pad));
  }
  if (refresh_button.width > 0) {
    left_max =
        std::min(left_max, std::max(0, refresh_button.x - (r.x + theme_.pad)));
  }
  // Keep the directory caption clear of the combos.
  if (widgets_ready_ && mode_combo_.widget.computed.width > 0) {
    left_max = std::min(
        left_max,
        std::max(0, mode_combo_.widget.computed.x - (r.x + theme_.pad)));
  }
  if (left_max > 0) {
    const std::string mode = (view_mode_ == ViewMode::ImageBrowser)
                                ? " [image]"
                                : " [usd]";
    DrawTextEllipsized(c, font_, r.x + theme_.pad, baseline, left_max,
                       status_left_ + mode, theme_.text);
  }
}

void App::DrawListPane(lvg_canvas_t* c, const lvg_rect_t& r) {
  FillRect(c, r, theme_.panel);

  const auto& entries = browser_.entries();
  if (entries.empty()) {
    DrawTextCentered(c, font_, r, "(empty folder)", theme_.text_dim);
    return;
  }

  lvg_canvas_set_clip(c, &r);

  const int rows = VisibleRows();
  const int first = browser_.scroll();
  const int ascent = lui_font_ascent(font_);

  for (int i = 0; i < rows; i++) {
    const int idx = first + i;
    if (idx >= static_cast<int>(entries.size())) break;
    const FileEntry& e = entries[static_cast<size_t>(idx)];

    const lvg_rect_t row =
        lvg_rect_make(r.x, r.y + i * theme_.row_h, r.width, theme_.row_h);

    if (idx == browser_.selected()) {
      FillRect(c, row, theme_.selection);
    } else if (idx & 1) {
      FillRect(c, row, theme_.panel_alt);
    }

    const int baseline = row.y + (theme_.row_h + ascent) / 2 - 2;

    // Right-hand column: size for files, nothing for directories.
    int size_w = 0;
    if (!e.is_dir) {
      const lvg_color_t size_color = e.over_budget ? theme_.warn
                                                   : theme_.text_dim;
      size_w = DrawTextRight(c, font_, row.x + row.width - theme_.pad, baseline,
                             FormatBytes(e.size), size_color);
      size_w += theme_.pad;
    }

    const std::string label = e.is_dir ? (e.name + "/") : e.name;
    lvg_color_t name_color = theme_.text;
    if (e.is_dir) {
      name_color = theme_.accent;
    } else if (e.over_budget) {
      name_color = theme_.warn;
    }

    const int name_max = row.width - 2 * theme_.pad - size_w;
    DrawTextEllipsized(c, font_, row.x + theme_.pad, baseline, name_max, label,
                       name_color);
  }

  lvg_canvas_reset_clip(c);

  // Scroll indicator: a thin thumb on the right edge, drawn only when the list
  // does not fit.
  const int total = static_cast<int>(entries.size());
  if (total > rows && rows > 0) {
    const int track_h = r.height;
    int thumb_h = std::max(16, track_h * rows / total);
    const int max_first = total - rows;
    const int thumb_y =
        r.y + (max_first > 0 ? (track_h - thumb_h) * first / max_first : 0);
    lvg_canvas_fill_rect(c, r.x + r.width - 3, thumb_y, 2, thumb_h,
                         theme_.text_dim);
  }
}

int App::VisibleRows() const {
  const Layout l = ComputeLayout(theme_, surf_w_, surf_h_, left_pane_w_);
  return std::max(0, l.list.height / theme_.row_h);
}

lvg_rect_t App::ResetButtonRect(const lvg_rect_t& toolbar) const {
  const int button_w = 64;
  const int button_h = std::max(16, toolbar.height - 2 * theme_.pad);
  const int required_text_w = 140;
  const int refresh_w = RefreshButtonRect(toolbar).width;
  const int reserved = button_w + (refresh_w > 0 ? theme_.pad + refresh_w : 0);

  const int available_left =
      std::max(0, toolbar.width - reserved - required_text_w - theme_.pad);
  if (available_left <= 0) return lvg_rect_t{};
  if (button_h <= 0) return lvg_rect_t{};

  if (refresh_w > 0) {
    return lvg_rect_make(toolbar.x + toolbar.width - reserved - theme_.pad,
                         toolbar.y + (toolbar.height - button_h) / 2,
                         button_w, button_h);
  }
  return lvg_rect_make(toolbar.x + toolbar.width - button_w - theme_.pad,
                       toolbar.y + (toolbar.height - button_h) / 2,
                       button_w, button_h);
}

lvg_rect_t App::RefreshButtonRect(const lvg_rect_t& toolbar) const {
  const int button_w = 72;
  const int button_h = std::max(16, toolbar.height - 2 * theme_.pad);
  const int required_text_w = 140;
  const int spacing = theme_.pad;
  const int needed = button_w + spacing + required_text_w;

  const int available_left = std::max(0, toolbar.width - needed - theme_.pad);
  if (available_left <= 0) return lvg_rect_t{};
  if (button_h <= 0) return lvg_rect_t{};

  return lvg_rect_make(toolbar.x + toolbar.width - button_w - theme_.pad,
                       toolbar.y + (toolbar.height - button_h) / 2,
                       button_w, button_h);
}

void App::RefreshFolder() {
  std::string berr;
  if (browser_.Refresh(&berr)) {
    status_left_ = browser_.dir();
    viewport_message_is_error_ = false;
    if (view_mode_ == ViewMode::ImageBrowser) {
      BuildImageItems();
    }
    if (const FileEntry* sel = browser_.SelectedEntry()) {
      if (view_mode_ == ViewMode::UsdPreview && !sel->is_dir) {
        PreviewFile(*sel);
      } else if (sel->is_dir && sel->over_budget) {
        status_right_ = "empty folder";
      }
    }
    needs_redraw_ = true;
    return;
  }

  viewport_message_ = berr;
  viewport_message_is_error_ = true;
  needs_redraw_ = true;
}

void App::ResetShadingAndViewport() {
  camera_ = OrbitCamera{};
  camera_goal_ = camera_;
  camera_animating_ = false;
  have_pick_ = false;
  if (scene_.bounds.valid) {
    camera_.y_up = scene_.y_up;
    camera_goal_.y_up = scene_.y_up;
  }

  camera_user_controlled_ = false;
  camera_framed_ = false;
  framed_at_mesh_count_ = 0;
  if (renderer_) {
    renderer_->SetScene(&scene_);
    renderer_->SetCamera(camera_);
  }
}

void App::ActivateSelection() {
  const FileEntry* sel = browser_.SelectedEntry();
  if (!sel) return;

  if (sel->is_dir) {
    std::string berr;
    if (browser_.DescendSelected(&berr)) {
      status_left_ = browser_.dir();
      if (view_mode_ == ViewMode::ImageBrowser) {
        BuildImageItems();
      }
      viewport_message_.clear();
      viewport_message_is_error_ = false;
      if (view_mode_ == ViewMode::UsdPreview) {
        if (const FileEntry* next = browser_.SelectedEntry()) {
          PreviewFile(*next);
        } else {
          viewport_message_ = "no USD files in this folder";
        }
      } else if (!browser_.SelectedEntry()) {
        viewport_message_ = "no files in this folder";
      }
    } else if (!berr.empty()) {
      viewport_message_ = berr;
      viewport_message_is_error_ = true;
    }
    needs_redraw_ = true;
    return;
  }

  PreviewFile(*sel);
  needs_redraw_ = true;
}

void App::PreviewFile(const FileEntry& entry) {
  if (entry.is_dir) return;

  viewport_message_is_error_ = false;

  if (entry.over_budget) {
    // Refused before opening: no allocation, no partial load. The user still
    // gets a specific reason and the numbers behind it.
    viewport_message_ = entry.name + " - too large for quick look (projected " +
                        FormatBytes(entry.projected_bytes) + ", budget " +
                        FormatBytes(budget_.total) + ")";
    viewport_message_is_error_ = true;
    status_right_ = "skipped (over budget)";
    return;
  }

  if (gl_budget_blocked_) {
    gl_budget_blocked_ = false;
    gl_error_.clear();
  }

  scene_.Clear();
  scene_complete_ = false;
  camera_user_controlled_ = false;
  camera_framed_ = false;
  framed_at_mesh_count_ = 0;
  if (renderer_) renderer_->SetScene(&scene_);
  viewport_message_ = entry.name;
  loader_.Start(entry.path, opts_, budget_);
  UpdateStatus();
}

bool App::StreamHasWork() const {
  LoadStream* stream = const_cast<App*>(this)->loader_.stream();
  return stream && stream->queued_bytes() > 0;
}

bool App::HasImageBrowserWork() const {
  if (view_mode_ != ViewMode::ImageBrowser) return false;
  std::lock_guard<std::mutex> lock(image_mu_);
  if (!image_task_queue_.empty()) return true;
  for (const ImageItem& item : image_items_) {
    if (item.status == ImageStatus::Pending ||
        item.status == ImageStatus::Loading) {
      return true;
    }
  }
  return false;
}

bool App::DrainLoadEvents() {
  LoadStream* stream = loader_.stream();
  if (!stream) return false;

  bool changed = false;
  LoadEvent ev;
  while (stream->TryPop(&ev)) {
    ApplyLoadEvent(std::move(ev));
    ev = LoadEvent{};
    changed = true;
  }

  if (changed) UpdateStatus();
  return changed;
}

bool App::DrainImageThumbnailEvents() {
  std::deque<ImageThumbnailEvent> pending;
  {
    std::lock_guard<std::mutex> lock(image_mu_);
    if (image_events_.empty()) return false;
    pending.swap(image_events_);
  }

  bool changed = false;
  while (!pending.empty()) {
    ApplyImageThumbnailEvent(std::move(pending.front()));
    pending.pop_front();
    changed = true;
  }
  return changed;
}

void App::ApplyImageThumbnailEvent(ImageThumbnailEvent&& ev) {
  if (ev.generation != image_generation_.load(std::memory_order_relaxed))
    return;

  {
    std::lock_guard<std::mutex> lock(image_mu_);
    if (ev.index >= image_items_.size()) return;

    ImageItem& item = image_items_[ev.index];
    if (ev.ok) {
      auto pixels = std::make_shared<std::vector<uint32_t>>(
          std::move(ev.argb_pixels));
      item.pixels = std::move(pixels);
      item.width = ev.width;
      item.height = ev.height;
      item.status = ImageStatus::Ready;
      item.error.clear();
    } else {
      item.status = ImageStatus::Error;
      item.error = std::move(ev.error);
      item.pixels.reset();
      item.width = 0;
      item.height = 0;
    }
  }
  UpdateImageStatus();
}

void App::ApplyLoadEvent(LoadEvent&& ev) {
  switch (ev.kind) {
    case LoadEvent::Kind::Progress:
      break;

    case LoadEvent::Kind::Resources:
      scene_.materials = std::move(ev.materials);
      scene_.textures = std::move(ev.textures);
      scene_.lights = std::move(ev.lights);
      scene_.cameras = std::move(ev.cameras);
      scene_.env_texture = ev.env_texture;
      for (int i = 0; i < QlScene::kEnvPrefilterLevels; i++) {
        scene_.env_prefiltered[i] = ev.env_prefiltered[i];
      }
      scene_.env_rotation = ev.env_rotation;
      scene_.env_intensity = ev.env_intensity;
      scene_.stats.material_count = scene_.materials.size();
      scene_.stats.texture_count = scene_.textures.size();
      scene_.stats.light_count = scene_.lights.size();
      break;

    case LoadEvent::Kind::Mesh:
      scene_.stats.triangle_count += ev.mesh.triangle_count();
      scene_.stats.vertex_count += ev.mesh.vertex_count();
      scene_.stats.geometry_bytes += ev.mesh.byte_size();
      scene_.bounds.Expand(ev.mesh.bounds);
      scene_.meshes.push_back(std::move(ev.mesh));
      scene_.stats.mesh_count = scene_.meshes.size();
      break;

    case LoadEvent::Kind::Bounds:
      if (ev.bounds.valid) scene_.bounds = ev.bounds;
      scene_.y_up = ev.y_up;
      break;

    case LoadEvent::Kind::Complete: {
      // Keep the counts accumulated from the mesh events: the worker's totals
      // cover geometry it converted, which is not always geometry we kept.
      const uint64_t tris = scene_.stats.triangle_count;
      const uint64_t verts = scene_.stats.vertex_count;
      const uint64_t gbytes = scene_.stats.geometry_bytes;
      const uint64_t mcount = scene_.stats.mesh_count;
      scene_.stats = ev.stats;
      scene_.stats.triangle_count = tris;
      scene_.stats.vertex_count = verts;
      scene_.stats.geometry_bytes = gbytes;
      scene_.stats.mesh_count = mcount;
      scene_.degraded = ev.degraded;
      if (ev.bounds.valid) scene_.bounds = ev.bounds;
      scene_.y_up = ev.y_up;
      scene_complete_ = true;
      if (scene_.meshes.empty() && !viewport_message_is_error_) {
        viewport_message_ = "no renderable geometry in this file";
        viewport_message_is_error_ = true;
      }
      break;
    }

    case LoadEvent::Kind::Failed:
      viewport_message_ = ev.error;
      viewport_message_is_error_ = true;
      scene_complete_ = true;
      break;
  }
}

void App::UpdateStatus() {
  if (view_mode_ == ViewMode::ImageBrowser) {
    UpdateImageStatus();
    return;
  }

  const LoadPhase phase = loader_.control_valid()
                             ? loader_.control().phase.load()
                             : LoadPhase::Idle;

  char buf[256];
  if (loader_.running() && phase != LoadPhase::Done) {
    const int permille = loader_.control().phase_permille.load();
    const uint32_t done = loader_.control().meshes_done.load();
    const uint32_t total = loader_.control().meshes_total.load();
    if (total > 0) {
      std::snprintf(buf, sizeof(buf), "%s %u/%u meshes", LoadPhaseName(phase),
                    done, total);
    } else {
      std::snprintf(buf, sizeof(buf), "%s %d%%", LoadPhaseName(phase),
                    permille / 10);
    }
    status_right_ = buf;
    return;
  }

  if (scene_.meshes.empty() && scene_complete_) {
    status_right_ = "empty";
    return;
  }
  if (scene_.meshes.empty()) {
    status_right_ = "ready";
    return;
  }

  std::snprintf(buf, sizeof(buf), "%s tris  %s meshes  %s mats  %s tex",
                FormatCount(scene_.stats.triangle_count).c_str(),
                FormatCount(scene_.stats.mesh_count).c_str(),
                FormatCount(scene_.stats.material_count).c_str(),
                FormatCount(scene_.stats.texture_count).c_str());
  status_right_ = buf;

  if (scene_.degraded.any()) {
    std::string why;
    if (scene_.degraded.uncomposed) why = "uncomposed";
    else if (scene_.degraded.triangle_cap_hit) why = "triangle cap";
    else if (scene_.degraded.proxy_geometry) why = "proxy geometry";
    else if (scene_.degraded.geometry_skipped) why = "geometry skipped";
    else if (scene_.degraded.textures_dropped) why = "textures dropped";
    status_right_ += "  [degraded: " + why + "]";
  }
}

void App::ToggleViewMode() {
  if (view_mode_ == ViewMode::UsdPreview) {
    view_mode_ = ViewMode::ImageBrowser;
    if (selected_image_ < 0 && !image_items_.empty()) selected_image_ = 0;
    UpdateImageStatus();
    status_left_ = browser_.dir();
  } else {
    view_mode_ = ViewMode::UsdPreview;
    viewport_message_is_error_ = false;
    status_left_ = browser_.dir();
    if (const FileEntry* sel = browser_.SelectedEntry()) {
      if (!sel->is_dir && !sel->over_budget) {
        viewport_message_ = sel->name;
      } else {
        viewport_message_ = "select a USD file to preview";
      }
    }
    UpdateStatus();
  }
  if (view_mode_ == ViewMode::ImageBrowser) {
    if (!image_workers_started_) {
      StartImageWorkers();
    }
    BuildImageItems();
  } else {
    // Keep image processing off while in USD preview mode.
    {
      std::lock_guard<std::mutex> lock(image_mu_);
      image_items_.clear();
      image_task_queue_.clear();
      image_events_.clear();
      selected_image_ = -1;
      image_scroll_ = 0;
    }
  }
  needs_redraw_ = true;
}

void App::EnterImageFolder(const std::string& dir_path) {
  std::string berr;
  if (!browser_.Open(dir_path, &berr)) {
    if (!berr.empty()) {
      viewport_message_ = berr;
      viewport_message_is_error_ = true;
    }
    needs_redraw_ = true;
    return;
  }
  status_left_ = browser_.dir();
  viewport_message_.clear();
  viewport_message_is_error_ = false;
  BuildImageItems();
  needs_redraw_ = true;
}

void App::BuildImageItems() {
  if (view_mode_ != ViewMode::ImageBrowser) return;
  if (headless_) return;
  EnsureImageCacheDir();

  // Always the immediate contents of the current folder -- mirrors the left
  // pane: subfolders are tiles you navigate into (EnterImageFolder), not
  // something to recurse into implicitly.
  const fs::path scan_root = fs::path(browser_.dir());
  std::vector<ImageItem> dirs;
  std::vector<ImageItem> images;
  std::error_code ec;

  fs::directory_iterator it(
      scan_root, fs::directory_options::skip_permission_denied, ec);
  if (ec) {
    viewport_message_ = "cannot read folder for images: " + browser_.dir();
    viewport_message_is_error_ = true;
    std::lock_guard<std::mutex> lock(image_mu_);
    image_items_.clear();
    image_task_queue_.clear();
    image_events_.clear();
    selected_image_ = -1;
    image_scroll_ = 0;
    return;
  }
  for (const auto& de : it) {
    std::error_code ec2;
    if (de.is_directory(ec2) && !ec2) {
      ImageItem item;
      item.nav_path = de.path().lexically_normal().string();
      item.name = de.path().filename().u8string();
      item.is_dir = true;
      // A representative image (if any turns up within a couple of levels)
      // becomes the tile's thumbnail; otherwise it falls back to a plain
      // folder icon. Cheap in practice: real trees are a handful of dirs
      // deep, not thousands wide.
      item.path = FindRepresentativeImage(de.path(), 2);
      if (item.path.empty()) {
        item.status = ImageStatus::Ready;  // no thumbnail to load
      } else {
        item.status = ImageStatus::Pending;
        item.cache_path = ImageCachePathForPath(item.path);
      }
      dirs.push_back(std::move(item));
      continue;
    }
    if (!de.is_regular_file(ec2) || ec2) continue;
    const std::string spath = de.path().lexically_normal().string();
    if (!IsImagePathInternal(spath)) continue;
    ImageItem item;
    item.path = spath;
    item.name = de.path().filename().u8string();
    item.status = ImageStatus::Pending;
    item.cache_path = ImageCachePathForPath(item.path);
    images.push_back(std::move(item));
  }

  auto by_name = [](const ImageItem& a, const ImageItem& b) {
    const std::string la = ToLower(a.name);
    const std::string lb = ToLower(b.name);
    if (la != lb) return la < lb;
    return a.name < b.name;
  };
  std::sort(dirs.begin(), dirs.end(), by_name);
  std::sort(images.begin(), images.end(), by_name);

  std::vector<ImageItem> items;
  items.reserve(dirs.size() + images.size() + 1);
  if (scan_root.has_parent_path() && scan_root.parent_path() != scan_root) {
    ImageItem up;
    up.nav_path = scan_root.parent_path().lexically_normal().string();
    up.name = "..";
    up.is_dir = true;
    up.status = ImageStatus::Ready;  // always a plain icon, no thumbnail
    items.push_back(std::move(up));
  }
  for (auto& d : dirs) items.push_back(std::move(d));
  for (auto& im : images) items.push_back(std::move(im));

  {
    std::lock_guard<std::mutex> lock(image_mu_);
    image_generation_.fetch_add(1, std::memory_order_relaxed);
    image_items_.clear();
    image_task_queue_.clear();
    image_events_.clear();

    const std::size_t generation = image_generation_.load(std::memory_order_relaxed);

    image_items_ = std::move(items);

    selected_image_ = image_items_.empty() ? -1 : 0;
    image_scroll_ = 0;

    for (size_t i = 0; i < image_items_.size(); i++) {
      // Dir tiles with no representative image (path left empty) and the
      // ".." tile have nothing to decode -- they stay Ready with no task.
      if (image_items_[i].path.empty()) continue;
      image_items_[i].status = ImageStatus::Loading;
      ImageTask task{i, generation, image_items_[i].path};
      image_task_queue_.push_back(std::move(task));
    }

    if (image_workers_started_) {
      image_cv_.notify_all();
    }
  }

  // UpdateImageStatus() takes image_mu_ itself; must run after the lock
  // above is released, not nested inside it (std::mutex isn't recursive).
  UpdateImageStatus();
}

void App::StartImageWorkers() {
  if (headless_ || image_workers_started_) return;
  image_workers_started_ = true;
  image_workers_stop_ = false;
  EnsureImageCacheDir();
  image_cache_disabled_.store(false);

  size_t hw = std::thread::hardware_concurrency();
  size_t nthreads = std::max<size_t>(1, hw / 4);
  if (nthreads == 0) nthreads = 1;

  image_workers_.reserve(nthreads);
  for (size_t i = 0; i < nthreads; i++) {
    image_workers_.emplace_back(&App::ThumbnailWorkerLoop, this);
  }
}

void App::StopImageWorkers() {
  if (!image_workers_started_) return;
  {
    std::lock_guard<std::mutex> lock(image_mu_);
    image_workers_stop_ = true;
  }
  image_cv_.notify_all();
  for (std::thread& t : image_workers_) {
    if (t.joinable()) t.join();
  }
  image_workers_.clear();
  image_workers_started_ = false;
  image_workers_stop_ = false;
  image_task_queue_.clear();
  image_events_.clear();
}

void App::ThumbnailWorkerLoop() {
  while (true) {
    ImageTask task;
    {
      std::unique_lock<std::mutex> lock(image_mu_);
      image_cv_.wait(lock, [this] {
        return image_workers_stop_ || !image_task_queue_.empty();
      });
      if (image_workers_stop_) return;
      if (image_task_queue_.empty()) continue;
      task = std::move(image_task_queue_.front());
      image_task_queue_.pop_front();
      if (task.generation != image_generation_.load(std::memory_order_relaxed)) continue;
      if (task.index < image_items_.size()) {
        image_items_[task.index].status = ImageStatus::Loading;
      }
    }

    ImageThumbnailEvent ev;
    ev.index = task.index;
    ev.generation = task.generation;
    ev.path = task.path;
    ev.ok = false;

    if (task.generation != image_generation_.load(std::memory_order_relaxed)) continue;

    std::string cache_path;
    {
      std::lock_guard<std::mutex> lock(image_mu_);
      if (task.index >= image_items_.size()) continue;
      cache_path = image_items_[task.index].cache_path;
    }

    DecodedImage image;
    bool got_thumbnail = false;
    if (!cache_path.empty() && LoadCachedThumbnail(cache_path, &image)) {
      got_thumbnail = true;
    }

    if (!got_thumbnail && LoadAndDownscaleImage(task.path, &image)) {
      got_thumbnail = true;
      if (!cache_path.empty() && !image_cache_disabled_.load()) {
        if (!SaveThumbnailToCache(cache_path, image)) {
          image_cache_disabled_.store(true);
          ev.error = "cache write skipped";
        }
      }
    }

    if (!got_thumbnail) {
      ev.error = "failed to decode image";
    } else {
      ev.ok = true;
      ev.width = static_cast<int>(image.width);
      ev.height = static_cast<int>(image.height);
      ev.argb_pixels.resize(image.rgba.size() / 4);
      for (size_t i = 0; i < image.rgba.size() / 4; i++) {
        const size_t s = i * 4;
        const uint8_t r = image.rgba[s];
        const uint8_t g = image.rgba[s + 1];
        const uint8_t b = image.rgba[s + 2];
        const uint8_t a = image.rgba[s + 3];
        ev.argb_pixels[i] = (uint32_t(a) << 24) | (uint32_t(r) << 16) |
                            (uint32_t(g) << 8) | uint32_t(b);
      }
    }

    {
      std::lock_guard<std::mutex> lock(image_mu_);
      image_events_.push_back(std::move(ev));
    }
  }
}

void App::EnsureImageCacheDir() {
  if (!image_cache_dir_.empty()) return;
  std::error_code ec;
  fs::path base = fs::temp_directory_path(ec);
  if (ec) return;
  base /= "lusdquicklook";
  base /= "cache";
  base /= "images";
  fs::create_directories(base, ec);
  if (ec) return;
  image_cache_dir_ = base.string();
  if (!image_cache_dir_.empty()) EnforceImageCacheLimit();
}

std::string App::ImageCacheDir() const { return image_cache_dir_; }

std::uint64_t App::Hash64(std::string_view s) const {
  const uint64_t kOffset = 1469598103934665603ull;
  const uint64_t kPrime = 1099511628211ull;
  uint64_t h = kOffset;
  for (unsigned char c : s) {
    h ^= c;
    h *= kPrime;
  }
  return h;
}

std::string App::ImageCachePathForPath(const std::string& path) const {
  std::error_code ec;
  const auto sz = fs::file_size(fs::path(path), ec);
  if (ec) return std::string();
  uint64_t mtime = 0;
  const auto t = fs::last_write_time(fs::path(path), ec);
  if (!ec) {
    mtime = static_cast<uint64_t>(t.time_since_epoch().count());
  }
  if (ImageCacheDir().empty()) return std::string();

  std::ostringstream key;
  key << path << "|" << sz << "|" << mtime;
  const std::uint64_t digest = Hash64(key.str());
  std::ostringstream hex;
  hex << std::hex << std::nouppercase << digest;
  fs::path p(image_cache_dir_);
  p /= (hex.str() + ".png");
  return p.string();
}

bool App::SpaceAvailableForCache(std::size_t bytes_needed) const {
  std::error_code ec;
  const fs::path dir = fs::path(ImageCacheDir());
  if (dir.empty()) return false;
  const fs::space_info si = fs::space(dir, ec);
  if (ec) return false;
  constexpr std::size_t kReserve = 64ull * 1024ull * 1024ull;
  return si.available >= kReserve + bytes_needed;
}

bool App::ReadFileBytes(const std::string& path, std::vector<uint8_t>* out,
                        uint64_t max_bytes) const {
  if (!out) return false;
  out->clear();
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) return false;
  ifs.seekg(0, std::ios::end);
  const std::streampos end = ifs.tellg();
  if (end <= 0) return false;
  if (static_cast<uint64_t>(end) > max_bytes) return false;
  ifs.seekg(0, std::ios::beg);
  out->resize(static_cast<size_t>(end));
  if (!ifs.read(reinterpret_cast<char*>(out->data()),
               static_cast<std::streamsize>(out->size()))) {
    return false;
  }
  return true;
}

bool App::LoadCachedThumbnail(const std::string& cache_path,
                              DecodedImage* out) const {
  if (cache_path.empty() || !out) return false;
  std::vector<uint8_t> bytes;
  if (!ReadFileBytes(cache_path, &bytes)) return false;
  return DecodeImageToRgba(bytes.data(), bytes.size(), kImageCacheMaxDim, out,
                           64ull << 20);
}

bool App::LoadAndDownscaleImage(const std::string& path, DecodedImage* out) const {
  if (!out) return false;
  std::vector<uint8_t> bytes;
  if (!ReadFileBytes(path, &bytes)) return false;
  return DecodeImageToRgba(bytes.data(), bytes.size(), kImageCacheMaxDim, out,
                           64ull << 20);
}

bool App::SaveThumbnailToCache(const std::string& cache_path,
                              const DecodedImage& image) const {
  if (cache_path.empty()) return false;
  if (image.width <= 0 || image.height <= 0 || image.rgba.empty()) return false;
  size_t texels = 0;
  size_t needed = 0;
  if (!budget_detail::CheckedMulSize(static_cast<size_t>(image.width),
                                     static_cast<size_t>(image.height),
                                     &texels) ||
      !budget_detail::CheckedMulSize(texels, 4, &needed) ||
      image.rgba.size() < needed) {
    return false;
  }
  if (!SpaceAvailableForCache(needed)) {
    return false;
  }

  std::vector<uint32_t> argb(texels);
  for (size_t i = 0; i < argb.size(); i++) {
    const size_t s = i * 4;
    const uint8_t r = image.rgba[s];
    const uint8_t g = image.rgba[s + 1];
    const uint8_t b = image.rgba[s + 2];
    const uint8_t a = image.rgba[s + 3];
    argb[i] = (uint32_t(a) << 24) | (uint32_t(r) << 16) |
              (uint32_t(g) << 8) | uint32_t(b);
  }

  lvg_surface_t surf = lvg_surface_wrap(argb.data(), int(image.width),
                                        int(image.height), int(image.width));
  if (lvg_surface_save_png(&surf, cache_path.c_str()) != 0) return false;
  EnforceImageCacheLimit();
  return true;
}

void App::EnforceImageCacheLimit() const {
  const std::string dir = ImageCacheDir();
  if (dir.empty()) return;
  std::error_code ec;
  std::vector<std::pair<fs::path, uint64_t>> files;
  uint64_t total = 0;
  for (const auto& de : fs::directory_iterator(dir, ec)) {
    if (ec) return;
    if (!de.is_regular_file(ec) || ec) continue;
    fs::path p = de.path();
    if (p.extension() != ".png") continue;
    const uint64_t sz = static_cast<uint64_t>(fs::file_size(p, ec));
    if (ec) continue;
    const uint64_t mtime =
        static_cast<uint64_t>(fs::last_write_time(p, ec).time_since_epoch().count());
    if (ec) continue;
    files.push_back({p, mtime});
    total = budget_detail::SaturatingAdd(total, sz);
  }
  if (total <= kImageCacheMaxBytes) return;

  std::sort(files.begin(), files.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });

  for (const auto& it : files) {
    if (total <= kImageCacheMaxBytes) break;
    const uint64_t sz = static_cast<uint64_t>(fs::file_size(it.first, ec));
    if (!ec) {
      fs::remove(it.first, ec);
      if (!ec) total = (total > sz) ? total - sz : 0;
    }
    ec.clear();
  }
}

void App::UpdateImageStatus() {
  std::vector<ImageStatus> statuses;
  std::lock_guard<std::mutex> lock(image_mu_);
  statuses.reserve(image_items_.size());
  for (const ImageItem& item : image_items_) {
    if (item.is_dir) continue;  // folder tiles aren't images
    statuses.push_back(item.status);
  }
  const bool cache_disabled = image_cache_disabled_.load();

  if (statuses.empty()) {
    status_right_ = "images: 0";
    if (status_left_.empty()) status_left_ = browser_.dir();
    return;
  }

  int ready = 0;
  for (ImageStatus s : statuses) {
    if (s == ImageStatus::Ready) ready++;
  }

  status_right_ = "images " + std::to_string(ready) + "/" +
                  std::to_string(statuses.size());
  if (cache_disabled) status_right_ += " [cache disabled]";

  const std::string cache_dir = ImageCacheDir();
  if (!cache_dir.empty()) {
    std::error_code ec;
    const fs::space_info si = fs::space(fs::path(cache_dir), ec);
    if (!ec) {
      status_right_ += "  cache " + FormatBytes(si.available) + " free";
    }
  }
}

bool App::MoveImageSelection(int delta) {
  std::size_t image_count = 0;
  {
    std::lock_guard<std::mutex> lock(image_mu_);
    image_count = image_items_.size();
  }
  if (image_count == 0) return false;

  const int old = selected_image_;
  const int next = std::max(
      0, std::min(static_cast<int>(image_count) - 1,
                                        selected_image_ + delta));
  if (next == selected_image_) return false;
  selected_image_ = next;

  const Layout l = ComputeLayout(theme_, surf_w_, surf_h_, left_pane_w_);
  const auto geo = ComputeImageGridGeometry(l.viewport.width, l.viewport.height);
  if (geo.cell_h <= 0 || geo.columns <= 0) return true;

  const int rows_visible =
      std::max(1, l.viewport.height / std::max(1, geo.cell_h));
  const int total_rows = static_cast<int>(
      (image_count + static_cast<size_t>(geo.columns) - 1) / geo.columns);
  const int max_scroll = std::max(0, total_rows - rows_visible);
  const int target_row = selected_image_ / geo.columns;
  if (target_row < image_scroll_) image_scroll_ = target_row;
  else if (target_row >= image_scroll_ + rows_visible)
    image_scroll_ = target_row - rows_visible + 1;
  if (image_scroll_ < 0) image_scroll_ = 0;
  if (image_scroll_ > max_scroll) image_scroll_ = max_scroll;
  return next != old;
}

bool App::ScrollImageByMouseWheel(int delta_y) {
  std::size_t image_count = 0;
  {
    std::lock_guard<std::mutex> lock(image_mu_);
    image_count = image_items_.size();
  }
  if (image_count == 0) return false;

  const Layout l = ComputeLayout(theme_, surf_w_, surf_h_, left_pane_w_);
  const auto geo = ComputeImageGridGeometry(l.viewport.width, l.viewport.height);
  if (geo.cell_h <= 0 || geo.columns <= 0) return false;
  const int rows_visible =
      std::max(1, l.viewport.height / std::max(1, geo.cell_h));
  const int total_rows = static_cast<int>(
      (image_count + static_cast<size_t>(geo.columns) - 1) / geo.columns);
  const int max_scroll = std::max(0, total_rows - rows_visible);
  const int next = std::max(0, std::min(max_scroll, image_scroll_ + delta_y));
  if (next == image_scroll_) return false;
  image_scroll_ = next;
  return true;
}

App::ImageGridGeometry App::ComputeImageGridGeometry(int viewport_w,
                                                    int viewport_h) const {
  ImageGridGeometry g;
  g.columns = std::max(kImageMinColumns, std::min(kImageMaxColumns, image_columns_));
  g.label_h = theme_.row_h;
  if (viewport_w <= 0) {
    g.cell_w = 0;
    g.cell_h = 0;
    g.image_sz = 0;
    return g;
  }
  g.cell_w = std::max(120, viewport_w / std::max(1, g.columns));
  g.image_sz = std::max(48, g.cell_w - 2 * theme_.pad);
  g.cell_h = g.image_sz + g.label_h + theme_.pad * 2;
  if (g.cell_h <= 0) g.cell_h = 0;
  return g;
}

void App::DrawSplitter(lvg_canvas_t* c, const lvg_rect_t& r) {
  FillRect(c, r, theme_.border);
  if (dragging_splitter_) FillRect(c, r, theme_.accent);
}

RenderSettings App::CurrentRenderSettings() const {
  RenderSettings rs;
  rs.spp = opts_.spp;
  rs.threads = ResolveThreadCount(opts_, !headless_);
  rs.shadows = shadows_enabled_;
  rs.ao = opts_.ao;
  rs.mode = shading_mode_;
  rs.ibl = ibl_enabled_;
  rs.exposure = exposure_;
  return rs;
}

void App::ApplyRenderSettings() {
  if (renderer_) renderer_->SetSettings(CurrentRenderSettings());
}

bool App::GlAffordable() const {
  // A GL driver costs ~100 MB of RSS that the budget cannot track (it lives in
  // the driver, not behind PoolAlloc). Under a tight cap that overhead dwarfs
  // the preview itself, so `auto` stays on the CPU; an explicit gl request
  // still gets what it asked for.
  return budget_.total >= (256ull << 20);
}

BackendChoice App::ResolveBackend() const {
  const bool gl_unavailable = gl_disabled_ || gl_budget_blocked_;
  if (desired_backend_ == BackendChoice::Auto) {
    return (GlAffordable() && !gl_unavailable) ? BackendChoice::Gl
                                               : BackendChoice::Cpu;
  }
  if (desired_backend_ == BackendChoice::Gl && gl_unavailable) {
    return BackendChoice::Cpu;
  }
  return desired_backend_;
}

bool App::CreateRenderer(BackendChoice backend, const lvg_rect_t& viewport,
                         std::string* err) {
  const RenderSettings rs = CurrentRenderSettings();
  std::unique_ptr<Renderer> next;

  if (backend == BackendChoice::Gl) {
    // Rough VRAM need: geometry + textures, plus the framebuffers.
    const uint64_t pixels = budget_detail::SaturatingMul(
        static_cast<uint64_t>(std::max(0, viewport.width)),
        static_cast<uint64_t>(std::max(0, viewport.height)));
    const uint64_t frame_bytes = budget_detail::SaturatingMul(pixels, 8);
    const uint64_t needed =
        budget_detail::SaturatingAdd(scene_.ByteSize(), frame_bytes);
    next = CreateGlRenderer(viewport.width, viewport.height, rs, needed,
                            opts_.max_gpu_mem_bytes, &gl_probe_, err);
    if (!next) return false;
  } else {
    next = CreateCpuRenderer(accel_);
    if (!next) {
      if (err) *err = "could not create the CPU renderer";
      return false;
    }
    if (!next->Init(viewport.width, viewport.height, rs, err)) return false;
  }

  // The renderer holds nothing that cannot be rebuilt from the app's own state,
  // which is what makes switching safe at any time — including mid-load.
  renderer_ = std::move(next);
  renderer_->SetScene(&scene_);
  renderer_->SyncScene();
  renderer_->SetCamera(camera_);
  renderer_->SetSettings(rs);
  renderer_->Resize(viewport.width, viewport.height);

  live_backend_ = backend;
  live_device_ = renderer_->DeviceName();
  return true;
}

void App::SwitchBackend(BackendChoice backend, const lvg_rect_t& viewport) {
  std::string err;
  // Release the old one first: an EGL context and its driver allocations should
  // not be held while the replacement asks for its own.
  renderer_.reset();

  if (CreateRenderer(backend, viewport, &err)) {
    if (backend == BackendChoice::Gl) gl_budget_blocked_ = false;
    if (opts_.verbose) {
      if (backend == BackendChoice::Gl && gl_probe_.available) {
        std::fprintf(stderr,
                     "[lusdquicklook] gl probe: %s / %s, free VRAM %llu MB, "
                     "estimate %llu MB, cap %llu MB\n",
                     gl_probe_.device.c_str(), gl_probe_.version.c_str(),
                     static_cast<unsigned long long>(gl_probe_.free_vram >> 20),
                     static_cast<unsigned long long>(gl_probe_.required_vram >> 20),
                     static_cast<unsigned long long>(gl_probe_.gpu_budget >> 20));
      }
      std::fprintf(stderr, "[lusdquicklook] renderer: %s (%s)\n",
                   renderer_->Name(), live_device_.c_str());
    }
    return;
  }

  if (backend == BackendChoice::Gl) {
    // A resource-cap refusal is recoverable when the user selects another
    // scene; context/driver failures are session-wide and should not be
    // retried every frame.
    const bool budget_failure = IsGlBudgetError(err);
    gl_budget_blocked_ = budget_failure;
    gl_disabled_ = !budget_failure;
    gl_error_ = err;
    if (desired_backend_ == BackendChoice::Gl || opts_.verbose) {
      std::fprintf(stderr, "[lusdquicklook] GL %s (%s); using the CPU renderer\n",
                   budget_failure ? "resource budget exceeded" : "backend unavailable",
                   err.c_str());
    }
    if (desired_backend_ == BackendChoice::Gl) {
      Notify((budget_failure ? "GL budget exceeded (" : "GL unavailable (") +
                 err + ")",
             LUI_TOAST_WARNING);
    }
    std::string cerr;
    if (CreateRenderer(BackendChoice::Cpu, viewport, &cerr) && opts_.verbose) {
      std::fprintf(stderr, "[lusdquicklook] renderer: %s (%s)\n",
                   renderer_->Name(), live_device_.c_str());
    }
    return;
  }

  std::fprintf(stderr, "[lusdquicklook] renderer unavailable: %s\n",
               err.c_str());
}

void App::DemoteToCpu(const std::string& why, const lvg_rect_t& viewport) {
  // Device loss is permanent for this session. A resource-cap refusal, on the
  // other hand, only blocks GL for the current scene.
  const bool budget_failure = IsGlBudgetError(why);
  gl_disabled_ = !budget_failure;
  gl_budget_blocked_ = budget_failure;
  gl_error_ = why;
  std::fprintf(stderr, "[lusdquicklook] GL %s (%s); falling back to the CPU "
                       "renderer\n",
               budget_failure ? "resource budget exceeded" : "backend lost",
               why.c_str());
  Notify((budget_failure ? "GL budget exceeded (" : "GL lost (") + why +
             ") \xE2\x80\x94 switched to CPU",
         LUI_TOAST_WARNING);
  renderer_.reset();
  std::string err;
  CreateRenderer(BackendChoice::Cpu, viewport, &err);
}

bool App::StepCameraMotion(float dt) {
  // Headless output must not depend on how many frames happened to elapse, and
  // --no-smoothing is the same request made explicitly.
  if (headless_ || !opts_.camera_smoothing) {
    camera_ = camera_goal_;
    camera_animating_ = false;
    return false;
  }

  const float radius = std::max(scene_.bounds.Radius(), 1e-4f);
  camera_animating_ = camera_.ApproachToward(camera_goal_, dt, radius);
  return camera_animating_;
}

bool App::PickAt(const lvg_rect_t& viewport, int x, int y) {
  if (!accel_ || viewport.width <= 0 || viewport.height <= 0) return false;

  if (accel_->scene() != &scene_) accel_->SetScene(&scene_);
  accel_->Sync(ResolveThreadCount(opts_, !headless_));

  PickHit hit;
  // The pick uses the displayed camera: the user aimed at what is on screen,
  // not at where a still-settling camera is heading.
  if (!accel_->Trace(camera_, x - viewport.x, y - viewport.y, viewport.width,
                     viewport.height, &hit)) {
    return false;
  }

  have_pick_ = true;
  pick_mesh_ = hit.mesh_index;
  for (int i = 0; i < 3; i++) pick_point_[i] = hit.position[i];

  camera_goal_.SetTarget(hit.position);
  camera_user_controlled_ = true;
  return true;
}

std::string App::BackendStatusText() const {
  std::string s = (live_backend_ == BackendChoice::Gl) ? "gl" : "cpu";
  if (!live_device_.empty()) {
    // GL_RENDERER carries transport/ISA noise ("NVIDIA GeForce RTX 5060
    // Ti/PCIe/SSE2"); the model name is the useful part.
    std::string device = live_device_;
    const size_t slash = device.find('/');
    if (slash != std::string::npos) device.resize(slash);
    s += " \xC2\xB7 " + device;
  }
  // Never let a silent demotion pass for a deliberate choice.
  if (live_backend_ != BackendChoice::Gl &&
      (gl_disabled_ || gl_budget_blocked_) &&
      desired_backend_ != BackendChoice::Cpu) {
    s += "  (gl: " + gl_error_ + ")";
  }
  return s;
}

void App::CycleBackend() {
  switch (desired_backend_) {
    case BackendChoice::Auto: desired_backend_ = BackendChoice::Cpu; break;
    case BackendChoice::Cpu:  desired_backend_ = BackendChoice::Gl;  break;
    case BackendChoice::Gl:   desired_backend_ = BackendChoice::Auto; break;
  }
  // A deliberate request to try GL again clears a previous failure: the user
  // may have fixed the driver, or just wants the error re-reported.
  if (desired_backend_ == BackendChoice::Gl) {
    gl_disabled_ = false;
    gl_budget_blocked_ = false;
    gl_error_.clear();
  }
}

bool App::EnsureRenderer(const lvg_rect_t& viewport) {
  if (viewport.width <= 0 || viewport.height <= 0) return false;

  // The CPU renderer and picking share the BVH. GL rasterization does not need
  // it, so defer the build until one of those paths actually requests it.
  if (accel_->scene() != &scene_) accel_->SetScene(&scene_);

  const BackendChoice wanted = ResolveBackend();
  if (!renderer_ || live_backend_ != wanted) {
    SwitchBackend(wanted, viewport);
    if (!renderer_) return false;
  }

  renderer_->Resize(viewport.width, viewport.height);
  return true;
}

void App::AutoFrameIfNeeded(const lvg_rect_t& viewport) {
  if (camera_user_controlled_) return;
  if (!scene_.bounds.valid) return;
  // Re-frame while geometry is still arriving: the bounds of the first mesh are
  // rarely the bounds of the scene.
  if (camera_framed_ && scene_.stats.mesh_count == framed_at_mesh_count_ &&
      scene_complete_) {
    return;
  }

  if (!camera_framed_) {
    camera_.y_up = scene_.y_up;
    // Default to a front-three-quarter view. "Front" differs by up axis: +Z for
    // Y-up stages, -Y for Z-up ones (the DCC convention most Z-up content is
    // authored to), so the same yaw would otherwise show the back of the model.
    camera_.yaw = scene_.y_up ? 0.7f : 3.1415927f - 0.7f;
    camera_.pitch = 0.32f;
  }
  const float aspect =
      viewport.height > 0 ? float(viewport.width) / float(viewport.height)
                          : 1.0f;
  if (!scene_.cameras.empty() && !camera_framed_) {
    camera_.FromDesc(scene_.cameras.front(), scene_.bounds);
  } else {
    camera_.FrameBounds(scene_.bounds, aspect);
  }
  camera_framed_ = true;
  framed_at_mesh_count_ = scene_.stats.mesh_count;
  // Auto-framing is not user motion: snap the goal to it rather than animating
  // toward a target that keeps moving as geometry streams in.
  camera_goal_ = camera_;
}

void App::DrawViewport(lvg_canvas_t* c, const lvg_rect_t& r) {
  if (!scene_.meshes.empty() && EnsureRenderer(r)) {
    AutoFrameIfNeeded(r);
    renderer_->SyncScene();
    renderer_->SetCamera(camera_);

    // Two budgets: generous when headless (the image is the deliverable),
    // tight when interactive (the UI is).
    const double budget_ms = headless_ ? 2000.0 : 12.0;
    render_status_ = renderer_->RenderStep(budget_ms);

    if (render_status_.device_lost) {
      // Swap the backend out and render the replacement's first frame now, so
      // the loss costs the user a frame rather than a blank viewport.
      DemoteToCpu(render_status_.error, r);
      if (renderer_) render_status_ = renderer_->RenderStep(budget_ms);
    }

    const uint32_t* px = renderer_ ? renderer_->Pixels() : nullptr;
    if (px && renderer_->width() == r.width &&
        renderer_->height() == r.height) {
      // The renderer already produces lightvg's native 0xAARRGGBB, so this is a
      // straight copy with no format conversion.
      lvg_surface_t src = lvg_surface_wrap(const_cast<uint32_t*>(px), r.width,
                                           r.height, r.width);
      lvg_canvas_blit(c, r.x, r.y, &src, nullptr);
      return;
    }
  }

  FillRectVGradient(c, r, theme_.viewport_top, theme_.viewport_bottom);

  const std::string msg = viewport_message_.empty()
                              ? std::string("select a USD file to preview")
                              : viewport_message_;
  const lvg_color_t color =
      viewport_message_is_error_ ? theme_.warn : theme_.text_dim;

  // Message cards can be long ("too large ..."); give them the pane width
  // minus a margin rather than letting the ellipsis eat the numbers.
  lvg_rect_t inner = r;
  inner.x += theme_.pad * 2;
  inner.width = std::max(0, r.width - theme_.pad * 4);
  DrawTextWrappedCentered(c, font_, inner, msg, color, 3);
}

void App::DrawImageBrowser(lvg_canvas_t* c, const lvg_rect_t& r) {
  FillRectVGradient(c, r, theme_.viewport_top, theme_.viewport_bottom);
  const ImageGridGeometry geo = ComputeImageGridGeometry(r.width, r.height);

  if (geo.cell_w <= 0 || geo.cell_h <= 0) {
    const std::string msg = "image browser cannot layout this window";
    DrawTextWrappedCentered(c, font_, r, msg, theme_.warn, 2);
    return;
  }

  std::size_t image_count = 0;
  {
    std::lock_guard<std::mutex> lock(image_mu_);
    image_count = image_items_.size();
  }
  if (image_count == 0) {
    // image_items_ includes subfolder tiles and "..", so this is a true
    // dead end: an empty folder at the filesystem root.
    const std::string msg = "this folder is empty";
    DrawTextWrappedCentered(c, font_, r, msg, theme_.text_dim, 2);
    return;
  }

  const int visible_rows = std::max(1, r.height / geo.cell_h);
  const int total_rows =
      static_cast<int>((image_count + geo.columns - 1) / geo.columns);
  const int max_scroll = std::max(0, total_rows - visible_rows);
  if (image_scroll_ < 0) image_scroll_ = 0;
  if (image_scroll_ > max_scroll) image_scroll_ = max_scroll;

  lvg_canvas_set_clip(c, &r);
  for (int row = 0; row < visible_rows; row++) {
    const int item_row = image_scroll_ + row;
    const int y = r.y + row * geo.cell_h;
    for (int col = 0; col < geo.columns; col++) {
      const int idx = item_row * geo.columns + col;
      if (idx >= static_cast<int>(image_count)) break;
      struct {
        std::string name;
        std::string error;
        ImageStatus status = ImageStatus::Pending;
        int width = 0;
        int height = 0;
        std::shared_ptr<std::vector<uint32_t>> pixels;
        bool is_dir = false;
      } item;

      {
        std::lock_guard<std::mutex> lock(image_mu_);
        if (idx >= static_cast<int>(image_items_.size())) break;
        const ImageItem& src = image_items_[static_cast<size_t>(idx)];
        item.name = src.name;
        item.error = src.error;
        item.status = src.status;
        item.width = src.width;
        item.height = src.height;
        item.pixels = src.pixels;
        item.is_dir = src.is_dir;
      }

      const int x = r.x + col * geo.cell_w;
      const lvg_rect_t cell =
          lvg_rect_make(x, y, geo.cell_w, geo.cell_h);

      if (idx == selected_image_) {
        FillRect(c, cell, theme_.selection);
      } else if ((item_row & 1) == 0 && (col & 1) == 0) {
        FillRect(c, cell, theme_.panel_alt);
      }

      const int slot_sz = std::max(20, geo.image_sz);
      const int image_x = cell.x + (geo.cell_w - geo.image_sz) / 2;
      const int image_y = cell.y + theme_.pad;

      if (item.is_dir) {
        const bool has_thumb = item.status == ImageStatus::Ready &&
                               item.pixels && !item.pixels->empty() &&
                               item.width > 0 && item.height > 0;
        if (has_thumb) {
          // Preview a representative image found inside the folder, same as
          // an image tile, so browsing a folder of folders still shows what
          // is in them -- with a small tab badge so it still reads as a
          // folder, not a picture.
          lvg_surface_t src = lvg_surface_wrap(item.pixels->data(),
                                               item.width, item.height,
                                               item.width);
          lvg_canvas_draw_image(c, image_x, image_y, geo.image_sz,
                                geo.image_sz, &src, nullptr,
                                LVG_IMAGE_FILTER_BILINEAR);
          StrokeRect(c, lvg_rect_make(image_x, image_y, slot_sz, slot_sz),
                    theme_.border);
          const int tab_w = slot_sz * 2 / 5;
          const int tab_h = std::max(4, slot_sz / 6);
          FillRect(c, lvg_rect_make(image_x, image_y, tab_w, tab_h),
                  theme_.accent);
        } else {
          // No image found inside (or still decoding one): a simple
          // two-rect folder pictogram (tab + body), matching the left
          // pane's convention of coloring directories with theme_.accent
          // rather than drawing a bitmap icon.
          const int tab_w = slot_sz * 2 / 5;
          const int tab_h = std::max(4, slot_sz / 6);
          FillRect(c, lvg_rect_make(image_x, image_y, tab_w, tab_h),
                  theme_.accent);
          FillRect(c, lvg_rect_make(image_x, image_y + tab_h, slot_sz,
                                   slot_sz - tab_h),
                  theme_.accent);
          StrokeRect(c, lvg_rect_make(image_x, image_y, slot_sz, slot_sz),
                    theme_.border);
        }

        const int text_y = y + geo.image_sz + theme_.pad +
                           (theme_.row_h - lui_font_ascent(font_)) / 2;
        DrawTextEllipsized(c, font_, x + theme_.pad, text_y,
                           std::max(0, geo.cell_w - 2 * theme_.pad),
                           item.name + "/", theme_.accent);
        continue;
      }

      FillRect(c, lvg_rect_make(image_x, image_y, slot_sz, slot_sz),
              item.status == ImageStatus::Ready ? theme_.panel_alt
                                               : theme_.panel);
      StrokeRect(c, lvg_rect_make(image_x, image_y, slot_sz, slot_sz),
                theme_.border);

      if (item.status == ImageStatus::Ready && item.pixels &&
          !item.pixels->empty() && item.width > 0 && item.height > 0) {
        lvg_surface_t src = lvg_surface_wrap(
            item.pixels->data(), item.width,
            item.height, item.width);
        lvg_canvas_draw_image(c, image_x, image_y, geo.image_sz, geo.image_sz,
                              &src, nullptr, LVG_IMAGE_FILTER_BILINEAR);
      } else {
        const std::string msg =
            item.status == ImageStatus::Error ? "!"
            : item.status == ImageStatus::Loading ? "..."
                                                : "";
        DrawTextCentered(c, font_,
                         lvg_rect_make(image_x, image_y, geo.image_sz, geo.image_sz),
                         msg, theme_.text_dim);
      }

      const int text_y =
          y + geo.image_sz + theme_.pad + (theme_.row_h - lui_font_ascent(font_)) / 2;
      lvg_color_t text_color = theme_.text;
      if (item.status == ImageStatus::Error) text_color = theme_.error;
      DrawTextEllipsized(c, font_, x + theme_.pad, text_y,
                         std::max(0, geo.cell_w - 2 * theme_.pad), item.name,
                         text_color);
      if (item.status == ImageStatus::Error && !item.error.empty()) {
        DrawTextEllipsized(c, font_, x + theme_.pad,
                           text_y + theme_.row_h,
                           std::max(0, geo.cell_w - 2 * theme_.pad),
                           item.error, theme_.warn);
      }
    }
  }
  lvg_canvas_reset_clip(c);

  DrawImageStatusOverlay(c, r);
}

void App::DrawImageStatusOverlay(lvg_canvas_t* c, const lvg_rect_t& r) {
  const std::string msg =
      image_cache_disabled_
          ? "thumbnail cache disabled (low disk)  [t] toggle USD preview"
          : "[t] toggle preview  [Enter] open folder  [+] columns  "
            "[ ] columns  [r] refresh";
  DrawTextCentered(c, font_, lvg_rect_make(r.x + theme_.pad, r.y + r.height - 24,
                                          r.width - theme_.pad * 2, 18),
                   msg, theme_.text_dim);
}

void App::DrawStatusBar(lvg_canvas_t* c, const lvg_rect_t& r) {
  // A real lui_statusbar_t: phase / live backend / memory, in three sections.
  // Its text is refreshed by SyncWidgetState() at the top of the frame.
  if (widgets_ready_ && statusbar_.widget.draw) {
    statusbar_.widget.draw(&statusbar_.widget, c);
    return;
  }

  // Headless never builds widgets, so it keeps the hand-drawn bar.
  FillRect(c, r, theme_.panel);
  lvg_canvas_fill_rect(c, r.x, r.y, r.width, 1, theme_.border);

  const int baseline = r.y + (r.height + lui_font_ascent(font_)) / 2 - 2;

  // Show tracked-vs-cap plus process RSS: the first is what the budget governs,
  // the second is the number that actually gets a process OOM-killed.
  const std::string right = "mem " + budget_.FormatUsage() + "  rss " +
                            FormatBytes(MemBudget::ProcessRSS());

  const int right_w = DrawTextRight(c, font_, r.x + r.width - theme_.pad,
                                    baseline, right, theme_.text_dim);
  // Leave a clear gap so the two halves never read as one run-on string on a
  // narrow window.
  const int left_max = r.width - 2 * theme_.pad - right_w - theme_.pad * 3;
  DrawTextEllipsized(c, font_, r.x + theme_.pad, baseline, left_max,
                     status_right_.empty() ? std::string("ready")
                                           : status_right_,
                     theme_.text_dim);
}

}  // namespace lusdql

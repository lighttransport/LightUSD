// ld3d12view - native Windows + Direct3D 12 USD viewer.
//
// Window, input and event loop come from lightui; scene loading and the camera
// are lmetalview's portable core (lmv_*) over the shared light3d OrbitCamera.
// D3D12 renders straight onto the lightui HWND, so lightui's own CPU present is
// disabled; the HUD is drawn with lightui into a CPU surface and composited.
// See README.md for the architecture, options and limitations.
//
// usage: ld3d12view [options] [file.usd[a|c|z]]
//
// Controls: left-drag orbit, right/middle-drag (or shift+left) pan, wheel dolly,
// F frame the scene, 1-7 view presets, W wireframe, G grid, S shadows,
// B background, H HUD, Space play/pause (animated scenes start playing),
// Left/Right step a frame, Home restart, L loop, C authored camera,
// Esc cancel a load (otherwise quit). Drop a USD file onto the window to open
// it; scenes load on a worker thread with a progress card.

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"

#include <lightui/lightui.h>
#include <lightui/window.h>

#include "d3d12/background.hh"
#include "d3d12/device.hh"
#include <shellapi.h>  // after windows.h (via device.hh)
#include "d3d12/grid.hh"
#include "d3d12/hud_renderer.hh"
#include "d3d12/mesh_renderer.hh"
#include "d3d12/points_lines.hh"
#include "d3d12/scene_gpu.hh"
#include "hud.hh"
#include "loader.hh"
#include "lmv.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

namespace {

struct Options {
  int width = 1280;
  int height = 720;
  int frames = 0;  // 0 = run until closed
  bool debug = false;
  bool warp = false;
  bool hidden = false;
  bool wireframe = false;
  bool grid = true;
  bool shadows = true;
  bool has_time = false;
  double time = 0.0;
  bool has_from = false;
  double from = 0.0;
  bool play = false;    // force playback even with --time
  bool hud = true;
  bool background = true;
  int load_delay_ms = 0;        // test hook: keep the load in flight
  bool capture_loading = false;  // test hook: screenshot while the load runs
  bool simulate_cancel = false;  // test hook: press Esc while loading
  bool paused = false;  // do not autoplay animated scenes
  bool loop = true;
  double speed = 1.0;
  bool authored_camera = false;
  bool simulate_space = false;  // test hook: post a Space key press
  int resize_w = 0, resize_h = 0;  // test hook: resize the window at frame 2
  std::string simulate_click;  // test hook: "play" or "timeline=F"
  std::string simulate_drop;  // test hook: post WM_DROPFILES for this path
  int preset = -1;  // 0..6 = iso, front, back, right, left, top, bottom
  std::string screenshot;
  std::string path;
};

void Usage() {
  std::fprintf(stderr,
               "usage: ld3d12view [--size WxH] [--frames N] [--debug] "
               "[--warp] [--hidden] [--wireframe] [--no-grid] [--no-shadows] [--preset 0-6]\n"
               "                   [--time T] [--animate-from T] [--play] [--paused] [--no-hud] [--no-background] [--load-delay-ms N] [--simulate-cancel] [--capture-loading] [--no-loop] "
               "[--speed X] [--authored-camera] [--simulate-drop file] [--simulate-space] [--simulate-click play|timeline=F] [--simulate-resize WxH] "
               "[--screenshot out.png] [file.usd]\n");
}

bool ParseArgs(int argc, char** argv, Options* o) {
  const char* env_debug = std::getenv("LD3D12VIEW_DEBUG");
  if (env_debug && *env_debug && *env_debug != '0') o->debug = true;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--size" && i + 1 < argc) {
      if (std::sscanf(argv[++i], "%dx%d", &o->width, &o->height) != 2 ||
          o->width < 16 || o->height < 16)
        return false;
    } else if (a == "--frames" && i + 1 < argc) {
      o->frames = std::atoi(argv[++i]);
    } else if (a == "--screenshot" && i + 1 < argc) {
      o->screenshot = argv[++i];
    } else if (a == "--debug") {
      o->debug = true;
    } else if (a == "--warp") {
      o->warp = true;
    } else if (a == "--hidden") {
      o->hidden = true;
    } else if (a == "--wireframe") {
      o->wireframe = true;
    } else if (a == "--preset" && i + 1 < argc) {
      o->preset = std::atoi(argv[++i]);
    } else if (a == "--time" && i + 1 < argc) {
      o->has_time = true;
      o->time = std::atof(argv[++i]);
    } else if (a == "--animate-from" && i + 1 < argc) {
      o->has_from = true;
      o->from = std::atof(argv[++i]);
    } else if (a == "--play") {
      o->play = true;
    } else if (a == "--load-delay-ms" && i + 1 < argc) {
      o->load_delay_ms = std::atoi(argv[++i]);
    } else if (a == "--capture-loading") {
      o->capture_loading = true;
    } else if (a == "--simulate-cancel") {
      o->simulate_cancel = true;
    } else if (a == "--no-background") {
      o->background = false;
    } else if (a == "--no-hud") {
      o->hud = false;
    } else if (a == "--paused") {
      o->paused = true;
    } else if (a == "--simulate-resize" && i + 1 < argc) {
      if (std::sscanf(argv[++i], "%dx%d", &o->resize_w, &o->resize_h) != 2)
        return false;
    } else if (a == "--simulate-click" && i + 1 < argc) {
      o->simulate_click = argv[++i];
    } else if (a == "--simulate-space") {
      o->simulate_space = true;
    } else if (a == "--no-loop") {
      o->loop = false;
    } else if (a == "--speed" && i + 1 < argc) {
      o->speed = std::atof(argv[++i]);
    } else if (a == "--simulate-drop" && i + 1 < argc) {
      o->simulate_drop = argv[++i];
    } else if (a == "--authored-camera") {
      o->authored_camera = true;
    } else if (a == "--no-shadows") {
      o->shadows = false;
    } else if (a == "--no-grid") {
      o->grid = false;
    } else if (!a.empty() && a[0] != '-' && o->path.empty()) {
      o->path = a;
    } else {
      return false;
    }
  }
  return true;
}

// C = A * B for column-major 4x4 matrices.
void Mul(const float* a, const float* b, float* c) {
  for (int col = 0; col < 4; ++col)
    for (int row = 0; row < 4; ++row) {
      float s = 0.0f;
      for (int k = 0; k < 4; ++k) s += a[k * 4 + row] * b[col * 4 + k];
      c[col * 4 + row] = s;
    }
}

// out = inverse of the column-major 4x4 `m`. Returns false when singular.
bool Invert4(const float* m, float* out) {
  float inv[16];
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
           m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
           m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
           m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
            m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
           m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
           m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
           m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
            m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
           m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
           m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
            m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
            m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
           m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
           m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
            m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
            m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (std::fabs(det) < 1e-30f) return false;
  const float inv_det = 1.0f / det;
  for (int i = 0; i < 16; ++i) out[i] = inv[i] * inv_det;
  return true;
}

struct SceneView {
  lmv_scene* scene = nullptr;
  float bmin[3] = {-4.0f, 0.0f, -4.0f};
  float bmax[3] = {4.0f, 2.0f, 4.0f};
  int up_axis = 0;
  float grid_model[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

void FrameScene(lmv_camera* camera, const SceneView& sv) {
  lmv_camera_frame_bounds(camera, sv.bmin, sv.bmax);
}

// Grid model matrix: power-of-ten cell size from the scene radius, rotated
// onto the XY plane for Z-up scenes (the grid itself is built on XZ).
void ComputeGridModel(SceneView* sv) {
  float r = 0.0f;
  for (int i = 0; i < 3; ++i) {
    const float e = 0.5f * (sv->bmax[i] - sv->bmin[i]);
    r += e * e;
  }
  r = std::sqrt(r);
  float s = 1.0f;
  if (r > 1e-6f) s = std::pow(10.0f, std::floor(std::log10(r / 5.0f)));
  const float zup[16] = {s, 0, 0, 0, 0, 0, s, 0, 0, -s, 0, 0, 0, 0, 0, 1};
  const float yup[16] = {s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, 0, 0, 0, 1};
  std::memcpy(sv->grid_model, sv->up_axis == 1 ? zup : yup,
              sizeof(sv->grid_model));
}

// Fills the view of an already loaded scene: bounds, up axis, grid, warnings.
void MakeSceneView(lmv_scene* scene, SceneView* sv) {
  sv->scene = scene;
  lmv_scene_bounds(scene, sv->bmin, sv->bmax);
  sv->up_axis = lmv_scene_up_axis(scene);
  ComputeGridModel(sv);
  for (uint32_t i = 0; i < lmv_scene_warning_count(scene); ++i)
    std::fprintf(stderr, "ld3d12view: warning: %s\n", lmv_scene_warning(scene, i));
  std::fprintf(stderr, "ld3d12view: %u mesh(es), %u draw(s), %u material(s)\n",
               lmv_scene_mesh_count(scene), lmv_scene_draw_count(scene),
               lmv_scene_material_count(scene));
}

// Layout of the shell's DROPFILES header (declared in shlobj_core.h).
struct DropFilesHeader {
  DWORD pFiles;
  POINT pt;
  BOOL fNC;
  BOOL fWide;
};

// Posts a WM_DROPFILES message carrying `utf8_path` to `hwnd`, the same message
// Explorer sends. DragFinish() in the window procedure frees the block.
bool PostSyntheticDrop(HWND hwnd, const std::string& utf8_path) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, nullptr, 0);
  if (n <= 0) return false;
  const size_t bytes = sizeof(DropFilesHeader) + (static_cast<size_t>(n) + 1) * sizeof(wchar_t);
  HGLOBAL h = GlobalAlloc(GHND, bytes);
  if (!h) return false;
  auto* df = static_cast<DropFilesHeader*>(GlobalLock(h));
  df->pFiles = sizeof(DropFilesHeader);
  df->fWide = TRUE;
  MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1,
                      reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(df) + sizeof(DropFilesHeader)), n);
  GlobalUnlock(h);
  if (!PostMessageW(hwnd, WM_DROPFILES, reinterpret_cast<WPARAM>(h), 0)) {
    GlobalFree(h);
    return false;
  }
  return true;
}

// Posts key down/up messages (what the keyboard driver sends) to `hwnd`.
void PostSyntheticClick(HWND hwnd, int x, int y) {
  const LPARAM pos = MAKELPARAM(static_cast<WORD>(x), static_cast<WORD>(y));
  PostMessageW(hwnd, WM_MOUSEMOVE, 0, pos);
  PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pos);
  PostMessageW(hwnd, WM_LBUTTONUP, 0, pos);
}

void PostSyntheticKey(HWND hwnd, WPARAM vk, UINT scancode) {
  const LPARAM down = 1 | static_cast<LPARAM>(scancode << 16);
  PostMessageW(hwnd, WM_KEYDOWN, vk, down);
  PostMessageW(hwnd, WM_KEYUP, vk, down | (1 << 30) | (1u << 31));
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!ParseArgs(argc, argv, &opt)) {
    Usage();
    return 2;
  }

  SceneView sv;  // filled when the first load finishes

  if (!lui_init()) {
    std::fprintf(stderr, "ld3d12view: lui_init failed\n");
    return 1;
  }
  uint32_t flags = LUI_WINDOW_RESIZABLE;
  if (opt.hidden) flags |= LUI_WINDOW_HIDDEN;
  lui_window_t* window =
      lui_window_create("ld3d12view", opt.width, opt.height, flags);
  if (!window) {
    std::fprintf(stderr, "ld3d12view: window creation failed\n");
    lui_shutdown();
    return 1;
  }
  lui_native_kind_t kind = LUI_NATIVE_NONE;
  const uintptr_t handle = lui_window_native_handle(window, &kind);
  if (kind != LUI_NATIVE_WIN32 || !handle) {
    std::fprintf(stderr, "ld3d12view: no Win32 window handle\n");
    lui_window_destroy(window);
    lui_shutdown();
    return 1;
  }
  lui_window_set_external_render(window, true);
  lui_window_set_file_drop(window, true);

  int exit_code = 0;
  {
    int pw = 0, ph = 0;
    lui_window_get_physical_size(window, &pw, &ph);

    ld3d12::Device device;
    ld3d12::GridRenderer grid;
    ld3d12::MeshRenderer meshes;
    ld3d12::PointLineRenderer points_lines;
    ld3d12::BackgroundRenderer background;
    ld3d12::Hud hud;
    ld3d12::HudRenderer hud_renderer;
    ld3d12::AsyncLoader loader;
    loader.set_test_delay_ms(opt.load_delay_ms);
    ld3d12::SceneGPU scene_gpu;
    std::string err;
    ld3d12::DeviceDesc dd;
    dd.hwnd = reinterpret_cast<HWND>(handle);
    dd.width = static_cast<UINT>(pw);
    dd.height = static_cast<UINT>(ph);
    dd.debug = opt.debug;
    dd.warp = opt.warp;
    if (!device.Init(dd, &err) || !grid.Init(device.device(), 10, &err) ||
        !meshes.Init(device.device(), &err) ||
        !background.Init(device.device(), &err) ||
        !points_lines.Init(device.device(), &err) || !hud.Init(&err) ||
        (hud.Resize(pw, ph), false) || !hud_renderer.Init(device.device(), &err) ||
        !hud_renderer.Resize(device.device(), static_cast<UINT>(pw),
                             static_cast<UINT>(ph), &err) ||
        (sv.scene && !scene_gpu.Build(device, sv.scene, &err))) {
      std::fprintf(stderr, "ld3d12view: %s\n", err.c_str());
      exit_code = 1;
    } else {
      std::fprintf(stderr, "ld3d12view: adapter \"%s\", %ux%u\n",
                   device.adapter_name().c_str(), device.width(),
                   device.height());
      if (opt.debug) {
        std::fprintf(stderr, "ld3d12view: debug layer %s\n",
                     device.debug_active()
                         ? "active"
                         : "unavailable (install the Graphics Tools feature)");
      }


      lmv_camera* camera = lmv_camera_create();
      lmv_camera_set_up_axis(camera, sv.up_axis);
      lmv_camera_set_aspect(camera, static_cast<float>(pw) /
                                        static_cast<float>(ph ? ph : 1));
      FrameScene(camera, sv);


      // ---- Animation ------------------------------------------------------
      bool animated = false;
      double t_start = 0.0, t_end = 0.0, tps = 24.0, cur_time = 0.0;
      uint32_t camera_count = 0;
      auto refresh_scene_info = [&] {
        animated = sv.scene && lmv_scene_has_animation(sv.scene);
        t_start = sv.scene ? lmv_scene_start_time(sv.scene) : 0.0;
        t_end = sv.scene ? lmv_scene_end_time(sv.scene) : 0.0;
        tps = sv.scene ? lmv_scene_frames_per_second(sv.scene) : 24.0;
        camera_count = sv.scene ? lmv_scene_camera_count(sv.scene) : 0;
        cur_time = sv.scene ? lmv_scene_time(sv.scene) : 0.0;
        if (sv.scene)
          std::fprintf(stderr,
                       "ld3d12view: animation %s, time [%g, %g] @ %g tps, "
                       "%u authored camera(s)\n",
                       animated ? "yes" : "no", t_start, t_end, tps,
                       camera_count);
      };
      refresh_scene_info();
      // Animated scenes play on open unless a time was requested explicitly.
      bool playing = animated && !opt.paused &&
                     (opt.play || (!opt.has_time && !opt.has_from));
      bool looping = opt.loop;
      bool use_authored = false;
      auto wall = [] {
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
      };
      double play_wall = wall(), play_t0 = cur_time;

      // Moves the scene to time `t`: the in-place update first (node, light,
      // camera, visibility, instancer changes), a full re-conversion plus GPU
      // rebuild when the animation touches topology or materials.
      auto apply_time = [&](double t) -> bool {
        if (!sv.scene) return true;
        char e[256] = {};
        lmv_status st = lmv_scene_update_animation(sv.scene, t, e, sizeof(e));
        if (st != LMV_OK) {
          st = lmv_scene_set_time(sv.scene, t, e, sizeof(e));
          if (st != LMV_OK) {
            std::fprintf(stderr, "ld3d12view: set_time(%g) failed: %s\n", t, e);
            return false;
          }
          device.WaitIdle();
          scene_gpu = ld3d12::SceneGPU();
          if (!scene_gpu.Build(device, sv.scene, &err)) {
            std::fprintf(stderr, "ld3d12view: %s\n", err.c_str());
            return false;
          }
        }
        cur_time = t;
        return true;
      };
      auto sync_authored_camera = [&] {
        if (!use_authored || !sv.scene || camera_count == 0) return;
        lmv_camera_view cv = {};
        if (lmv_scene_camera(sv.scene, 0, &cv) == LMV_OK)
          lmv_camera_set_from_camera(camera, &cv);
      };
      auto leave_authored_camera = [&] { use_authored = false; };
      // The core loads at time code 0, which can lie outside the authored clip
      // (e.g. 1..48); move into the clip so playback and the first frame are
      // valid. Skipped when the caller asked for explicit times.
      auto enter_clip = [&](bool initial) {
        if (animated && (!initial || (!opt.has_time && !opt.has_from)) &&
            (cur_time < t_start || cur_time > t_end))
          apply_time(t_start);
      };
      auto seek = [&](double t) {
        if (t_end > t_start) t = std::min(std::max(t, t_start), t_end);
        playing = false;
        if (!apply_time(t)) exit_code = 1;
      };
      bool setup_ok = true;
      play_wall = wall();
      play_t0 = cur_time;
      double fps_wall = wall();
      int fps_frames = 0;
      auto base_name = [](const std::string& path) {
        return path.empty() ? std::string("ld3d12view")
                            : path.substr(path.find_last_of("/\\") + 1);
      };
      std::string title_name = base_name(opt.path);
      // Brief error message in the status panel after a failed load.
      std::string error_text;
      double error_until = 0.0;
      std::string info_text;
      double last_fps = 0.0;
      auto update_info = [&] {
        char b[96];
        if (!error_text.empty() && wall() < error_until) {
          info_text = error_text;
        } else if (!sv.scene) {
          info_text = loader.Busy() ? "loading..." : "drop a USD file to open";
        } else if (last_fps > 0.0) {
          std::snprintf(b, sizeof(b), animated ? "%u mesh(es)  %.0f tps  %.0f fps"
                                               : "%u mesh(es)  %.0f fps",
                        lmv_scene_mesh_count(sv.scene),
                        animated ? tps : last_fps, last_fps);
          info_text = b;
        } else {
          std::snprintf(b, sizeof(b), "%u mesh(es)", lmv_scene_mesh_count(sv.scene));
          info_text = b;
        }
      };
      update_info();

      // Installs a freshly loaded scene (main thread): uploads it first, so a
      // failed upload leaves the current scene alone. `initial` marks the
      // command-line scene, which also gets the --time/--preset/camera options.
      bool initial_pending = !opt.path.empty();
      auto install_scene = [&](lmv_scene* scene, const std::string& path,
                               bool initial) -> bool {
        SceneView next;
        MakeSceneView(scene, &next);
        device.WaitIdle();  // the old scene's buffers may still be in flight
        ld3d12::SceneGPU fresh;
        if (!fresh.Build(device, next.scene, &err)) {
          std::fprintf(stderr, "ld3d12view: %s\n", err.c_str());
          lmv_scene_free(next.scene);
          return false;
        }
        scene_gpu = std::move(fresh);
        background.SetEnvironment(device.device(), scene_gpu.env_resource());
        if (sv.scene) lmv_scene_free(sv.scene);
        sv = next;
        std::fprintf(stderr,
                       "ld3d12view: %u texture(s) uploaded, %u unsupported; "
                       "%zu %s light(s)%s%s\n",
                       scene_gpu.texture_count, scene_gpu.unsupported_textures,
                       scene_gpu.lights.size(),
                       scene_gpu.authored_lights ? "authored" : "studio",
                       scene_gpu.env_index >= 0 ? ", dome environment" : "",
                       scene_gpu.shadow_light >= 0 ? ", shadows" : "");
        refresh_scene_info();
        enter_clip(initial);
        bool ok = true;
        if (initial && opt.has_from) ok = apply_time(opt.from) && ok;
        if (initial && opt.has_time) ok = apply_time(opt.time) && ok;
        playing = animated && !opt.paused &&
                  (!initial || opt.play || (!opt.has_time && !opt.has_from));
        use_authored = false;
        play_wall = wall();
        play_t0 = cur_time;
        lmv_camera_set_up_axis(camera, sv.up_axis);
        FrameScene(camera, sv);
        if (initial && opt.preset >= 0) lmv_camera_set_preset(camera, opt.preset);
        if (initial && opt.authored_camera && camera_count > 0) {
          use_authored = true;
          sync_authored_camera();
        }
        title_name = base_name(path);
        update_info();
        lui_window_set_title(window, title_name.c_str());
        std::fprintf(stderr, "ld3d12view: opened %s\n", path.c_str());
        return ok;
      };
      auto request_load = [&](const std::string& path) {
        std::fprintf(stderr, "ld3d12view: loading %s\n", path.c_str());
        loader.Request(path);
        update_info();
      };
      auto toggle_play = [&] {
        if (!animated) {
          std::fprintf(stderr, "ld3d12view: no animation in this scene\n");
          return;
        }
        if (!playing && !looping && cur_time >= t_end - 1e-9) apply_time(t_start);
        playing = !playing;
        play_wall = wall();
        play_t0 = cur_time;
        std::fprintf(stderr, "ld3d12view: %s at t=%.2f\n",
                     playing ? "playing" : "paused", cur_time);
      };
      hud.on_toggle_play = [&] { toggle_play(); };
      hud.on_seek = [&](double t) {
        seek(t);
        std::fprintf(stderr, "ld3d12view: timeline seek to t=%.2f\n", cur_time);
      };
      hud.SetVisible(opt.hud);

      bool left = false, right = false, middle = false;
      bool wireframe = opt.wireframe;
      bool show_grid = opt.grid;
      bool show_background = opt.background;
      bool shadows = opt.shadows;
      bool quit = !setup_ok;
      int frames = 0;
      int iteration = 0;
      int logged_phase = -1;
      if (!opt.path.empty()) request_load(opt.path);
      const float clear[4] = {0.10f, 0.11f, 0.13f, 1.0f};

      while (!quit) {
        lui_event_t ev;
        while (lui_window_poll_event(window, &ev)) {
          switch (ev.type) {
            case LUI_EVENT_QUIT:
              quit = true;
              break;
            case LUI_EVENT_WINDOW_RESIZE: {
              lui_window_get_physical_size(window, &pw, &ph);
              if (pw > 0 && ph > 0) {
                if (!device.Resize(static_cast<UINT>(pw),
                                   static_cast<UINT>(ph), &err)) {
                  std::fprintf(stderr, "ld3d12view: %s\n", err.c_str());
                  exit_code = 1;
                  quit = true;
                }
                lmv_camera_set_aspect(camera, static_cast<float>(pw) /
                                                  static_cast<float>(ph));
                hud.Resize(pw, ph);
                std::fprintf(stderr, "ld3d12view: resized to %dx%d\n", pw, ph);
                if (!hud_renderer.Resize(device.device(), static_cast<UINT>(pw),
                                         static_cast<UINT>(ph), &err)) {
                  std::fprintf(stderr, "ld3d12view: %s\n", err.c_str());
                  exit_code = 1;
                  quit = true;
                }
              }
              break;
            }
            case LUI_EVENT_KEY_DOWN:
              if (ev.data.key.key == LUI_KEY_ESCAPE) {
                if (loader.Busy()) {
                  loader.Cancel();
                  std::fprintf(stderr, "ld3d12view: cancelling load\n");
                } else {
                  quit = true;
                }
              }
              if (ev.data.key.key == 'f' || ev.data.key.key == 'F')
                FrameScene(camera, sv);
              if (ev.data.key.key == 'w' || ev.data.key.key == 'W')
                wireframe = !wireframe;
              if (ev.data.key.key >= '1' && ev.data.key.key <= '7')
                lmv_camera_set_preset(camera, ev.data.key.key - '1');
              if (ev.data.key.key == 's' || ev.data.key.key == 'S')
                shadows = !shadows;
              if (ev.data.key.key == 'g' || ev.data.key.key == 'G')
                show_grid = !show_grid;
              if (ev.data.key.key == 'b' || ev.data.key.key == 'B')
                show_background = !show_background;
              if (ev.data.key.key == ' ') toggle_play();
              if (ev.data.key.key == 'h' || ev.data.key.key == 'H')
                hud.SetVisible(!hud.visible());
              if (ev.data.key.key == LUI_KEY_RIGHT && animated)
                seek(cur_time + 1.0);
              if (ev.data.key.key == LUI_KEY_LEFT && animated)
                seek(cur_time - 1.0);
              if (ev.data.key.key == LUI_KEY_HOME && animated) seek(t_start);
              if (ev.data.key.key == 'l' || ev.data.key.key == 'L')
                looping = !looping;
              if ((ev.data.key.key == 'c' || ev.data.key.key == 'C') &&
                  camera_count > 0) {
                use_authored = !use_authored;
                sync_authored_camera();
              }
              break;
            case LUI_EVENT_FILE_DROP: {
              // One event per dropped file; open the first, discard the rest.
              std::string first;
              while (char* dropped = lui_window_take_dropped_file(window)) {
                if (first.empty()) first = dropped;
                std::free(dropped);
              }
              if (!first.empty()) request_load(first);
              break;
            }
            case LUI_EVENT_MOUSE_DOWN:
            case LUI_EVENT_MOUSE_UP: {
              const bool down = ev.type == LUI_EVENT_MOUSE_DOWN;
              const bool hud_took = hud.HandleEvent(ev);
              // A press on the HUD must not start a camera drag; a release always
              // ends one, wherever the pointer is.
              if (down && hud_took) break;
              const auto b = ev.data.mouse_button.button;
              if (b == LUI_MOUSE_LEFT) left = down;
              if (b == LUI_MOUSE_RIGHT) right = down;
              if (b == LUI_MOUSE_MIDDLE) middle = down;
              break;
            }
            case LUI_EVENT_MOUSE_MOVE: {
              if (hud.HandleEvent(ev)) break;
              const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
              const float dx = static_cast<float>(ev.data.mouse_move.dx);
              const float dy = static_cast<float>(ev.data.mouse_move.dy);
              if (right || middle || (left && shift)) {
                leave_authored_camera();
                lmv_camera_pan(camera, dx, dy);
              } else if (left) {
                leave_authored_camera();
                lmv_camera_orbit(camera, dx, dy);
              }
              break;
            }
            case LUI_EVENT_SCROLL:
              if (hud.HandleEvent(ev)) break;
              leave_authored_camera();
              lmv_camera_dolly(camera, -ev.data.scroll.delta_y * 0.6f);
              break;
            default:
              break;
          }
        }
        // Finished loads: upload on this (the GPU) thread.
        ld3d12::LoadResult lr;
        while (loader.Poll(&lr)) {
          const bool initial = initial_pending && lr.path == opt.path;
          if (initial) initial_pending = false;
          if (lr.cancelled) {
            std::fprintf(stderr, "ld3d12view: load cancelled: %s\n", lr.path.c_str());
          } else if (lr.status != LMV_OK) {
            std::fprintf(stderr, "ld3d12view: cannot load %s: %s\n",
                         lr.path.c_str(), lr.error.c_str());
            error_text = "failed to load " + base_name(lr.path);
            error_until = wall() + 6.0;
            update_info();
            if (initial) {
              exit_code = 1;
              quit = true;
            }
          } else if (!install_scene(lr.scene, lr.path, initial) && initial) {
            exit_code = 1;
            quit = true;
          }
        }
        if (quit) break;
        if (pw <= 0 || ph <= 0) {  // minimized
          Sleep(16);
          continue;
        }

        ++iteration;
        if (opt.simulate_cancel && iteration == 6)
          PostSyntheticKey(reinterpret_cast<HWND>(handle), VK_ESCAPE, 0x01);
        if (playing) {
          const double span = t_end - t_start;
          double t = play_t0 + (wall() - play_wall) * opt.speed * tps;
          if (t > t_end) {
            if (!looping || span <= 1e-9) {
              t = t_end;
              playing = false;
            } else {
              t = t_start + std::fmod(t - t_start, span);
            }
          }
          if (t != cur_time && !apply_time(t)) {
            exit_code = 1;
            break;
          }
        }
        sync_authored_camera();

        lmv_camera_frame cf = {};
        lmv_camera_get_frame(camera, &cf);
        ld3d12::FrameParams fp;
        Mul(cf.proj, cf.view, fp.view_proj);
        std::memcpy(fp.eye, cf.eye, sizeof(fp.eye));
        fp.wireframe = wireframe;
        fp.shadows = shadows;
        fp.exposure_stops = cf.exposure;

        ID3D12GraphicsCommandList* list = device.BeginFrame(clear);
        if (!list) {
          std::fprintf(stderr, "ld3d12view: BeginFrame failed\n");
          exit_code = 1;
          break;
        }
        if (show_background) {
          ld3d12::BackgroundParams bp;
          if (Invert4(fp.view_proj, bp.inv_view_proj)) {
            std::memcpy(bp.eye, cf.eye, sizeof(bp.eye));
            bp.exposure_stops = cf.exposure;
            bp.up_axis = sv.up_axis;
            if (sv.scene)
              std::memcpy(bp.color_matrix, scene_gpu.color_matrix,
                          sizeof(bp.color_matrix));
            background.Draw(list, bp);
          }
        }
        if (sv.scene) {
          scene_gpu.RefreshLights(sv.scene, device.frame_index());
          scene_gpu.UpdateDeform(sv.scene, device.frame_index());
          meshes.Draw(device, list, fp, sv.scene, scene_gpu);
        }
        if (sv.scene && (scene_gpu.splat_count || scene_gpu.line_count)) {
          ld3d12::PointLineParams pl;
          std::memcpy(pl.view_proj, fp.view_proj, sizeof(pl.view_proj));
          std::memcpy(pl.view, cf.view, sizeof(pl.view));
          std::memcpy(pl.eye, cf.eye, sizeof(pl.eye));
          pl.pixel_scale = cf.orthographic
                               ? 0.0f
                               : 2.0f * std::tan(0.5f * cf.fov_y) /
                                     static_cast<float>(ph);
          points_lines.Draw(list, pl, scene_gpu);
        }
        if (show_grid) {
          float grid_vp[16];
          Mul(fp.view_proj, sv.grid_model, grid_vp);
          grid.Draw(list, grid_vp);
        }

        {
          const ld3d12::LoadProgress lp = loader.Progress();
          ld3d12::HudLoading hl;
          hl.active = lp.active;
          hl.cancelling = lp.cancelling;
          hl.title = base_name(lp.path);
          hl.progress = lp.overall;
          const char* phase_name = "Working";
          switch (lp.phase) {
            case LMV_PHASE_OPEN: phase_name = "Opening"; break;
            case LMV_PHASE_COMPOSE: phase_name = "Composing stage"; break;
            case LMV_PHASE_CONVERT: phase_name = "Converting geometry and materials"; break;
            case LMV_PHASE_TEXTURES: phase_name = "Decoding textures"; break;
            default: break;
          }
          hl.message = phase_name;
          if (!lp.message.empty()) hl.message += "  (" + lp.message + ")";
          hud.SetLoading(hl);
          if (lp.active && lp.phase != logged_phase) {
            logged_phase = lp.phase;
            std::fprintf(stderr, "ld3d12view: loading %s: %s\n", hl.title.c_str(), phase_name);
          } else if (!lp.active) {
            logged_phase = -1;
          }
        }
        {
          ld3d12::HudStatus hs;
          hs.title = title_name;
          hs.info = info_text;
          hs.animated = animated;
          hs.playing = playing;
          hs.time = cur_time;
          hs.t_start = t_start;
          hs.t_end = t_end;
          hs.tps = tps;
          hud.SetStatus(hs);
          const std::vector<ld3d12::HudRows> rows = hud.Render();
          hud_renderer.Upload(list, device.frame_index(), hud.surface()->pixels,
                              hud.surface()->stride, rows);
          if (hud.visible()) hud_renderer.Draw(list, 0.92f);
        }

        if (iteration == 3 && !opt.simulate_drop.empty() &&
            !PostSyntheticDrop(reinterpret_cast<HWND>(handle),
                               opt.simulate_drop)) {
          std::fprintf(stderr, "ld3d12view: could not post the drop message\n");
          exit_code = 1;
        }
        if (frames == 2 && opt.resize_w > 0)
          SetWindowPos(reinterpret_cast<HWND>(handle), nullptr, 0, 0,
                       opt.resize_w, opt.resize_h, SWP_NOMOVE | SWP_NOZORDER);
        if (frames == 3 && !opt.simulate_click.empty()) {
          int cx = 0, cy = 0;
          const bool ok =
              opt.simulate_click == "play"
                  ? hud.PlayButtonPoint(&cx, &cy)
                  : hud.TimelinePoint(
                        std::atof(opt.simulate_click.c_str() +
                                  (opt.simulate_click.find('=') + 1)),
                        &cx, &cy);
          if (ok) {
            PostSyntheticClick(reinterpret_cast<HWND>(handle), cx, cy);
          } else {
            std::fprintf(stderr, "ld3d12view: no HUD bar to click\n");
            exit_code = 1;
          }
        }
        if (frames == 3 && opt.simulate_space)
          PostSyntheticKey(reinterpret_cast<HWND>(handle), VK_SPACE, 0x39);
        if (!loader.Busy()) ++frames;
        ++fps_frames;
        if (const double now = wall(); now - fps_wall >= 0.5) {
          char title[256];
          if (animated)
            std::snprintf(title, sizeof(title),
                          "%s  t=%.2f [%.2f..%.2f]%s%s  %.0f fps",
                          title_name.c_str(), cur_time, t_start, t_end,
                          playing ? " playing" : " paused", use_authored ? " cam" : "",
                          fps_frames / (now - fps_wall));
          else
            std::snprintf(title, sizeof(title), "%s  %.0f fps",
                          title_name.c_str(), fps_frames / (now - fps_wall));
          lui_window_set_title(window, title);
          last_fps = fps_frames / (now - fps_wall);
          update_info();
          fps_wall = now;
          fps_frames = 0;
        }
        const bool mid_load = opt.capture_loading && loader.Busy() && iteration >= 12;
        const bool last = mid_load || (opt.frames > 0 && frames >= opt.frames);
        ld3d12::Image shot;
        const bool capture = last && !opt.screenshot.empty();
        if (!device.EndFrame(capture ? &shot : nullptr, &err)) {
          std::fprintf(stderr, "ld3d12view: %s\n", err.c_str());
          exit_code = 1;
          break;
        }
        if (capture) {
          if (!stbi_write_png(opt.screenshot.c_str(),
                              static_cast<int>(shot.width),
                              static_cast<int>(shot.height), 4,
                              shot.rgba.data(),
                              static_cast<int>(shot.width) * 4)) {
            std::fprintf(stderr, "ld3d12view: cannot write %s\n",
                         opt.screenshot.c_str());
            exit_code = 1;
          }
        }
        if (last) break;
      }
      lmv_camera_destroy(camera);
    }
    device.WaitIdle();
    if (opt.debug && device.debug_active()) {
      const uint32_t errors = device.DrainValidation();
      std::fprintf(stderr, "ld3d12view: debug layer: %u validation error(s)\n",
                   errors);
      if (errors) exit_code = 1;
    }
  }

  if (sv.scene) lmv_scene_free(sv.scene);
  lui_window_destroy(window);
  lui_shutdown();
  return exit_code;
}

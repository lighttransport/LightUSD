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
#include "d3d12/mathutil.hh"
#include "d3d12/mesh_renderer.hh"
#include "d3d12/points_lines.hh"
#include "d3d12/scene_gpu.hh"
#include "hud.hh"
#include "loader.hh"
#include "lmv.h"
#include "pick.hh"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <map>
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
  bool compress_textures = true;  // BC7-encode large 8-bit images at load
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
  std::string simulate_pick;   // test hook: click at "fx,fy" (window fractions)
  bool outliner = false;       // start with the outliner panel open
  int camera_index = 0;        // --camera N: authored camera for --authored-camera
  int simulate_camera_cycle = 0;   // test hook: press C this many times
  int simulate_bookmark = 0;       // test hook: save, disturb and recall slot N
  int simulate_bookmark_load = 0;  // test hook: recall slot N (persisted)
  ld3d12::VariantOverrides variants;  // --variant set=name (repeatable)
  int simulate_variant_click = -1;    // test hook: click variant row N
  int simulate_outline_click = -1;    // test hook: click prim row N
  std::string simulate_drop;  // test hook: post WM_DROPFILES for this path
  int preset = -1;  // 0..6 = iso, front, back, right, left, top, bottom
  std::string screenshot;
  std::string path;
};

void Usage() {
  std::fprintf(stderr,
               "usage: ld3d12view [--size WxH] [--frames N] [--debug] "
               "[--warp] [--hidden] [--wireframe] [--no-grid] [--no-shadows] [--no-compress-textures] [--preset 0-6]\n"
               "                   [--time T] [--animate-from T] [--play] [--paused] [--no-hud] [--no-background] [--load-delay-ms N] [--simulate-cancel] [--capture-loading] [--no-loop] "
               "[--speed X] [--authored-camera] [--simulate-drop file] [--simulate-space] [--simulate-click play|timeline=F] [--simulate-pick fx,fy] [--outliner] [--variant set=name] [--camera N] [--simulate-resize WxH] "
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
    } else if (a == "--camera" && i + 1 < argc) {
      o->authored_camera = true;
      o->camera_index = std::max(0, std::atoi(argv[++i]));
    } else if (a == "--simulate-camera-cycle" && i + 1 < argc) {
      o->simulate_camera_cycle = std::atoi(argv[++i]);
    } else if (a == "--simulate-bookmark" && i + 1 < argc) {
      o->simulate_bookmark = std::atoi(argv[++i]);
    } else if (a == "--simulate-bookmark-load" && i + 1 < argc) {
      o->simulate_bookmark_load = std::atoi(argv[++i]);
    } else if (a == "--outliner") {
      o->outliner = true;
    } else if (a == "--variant" && i + 1 < argc) {
      const std::string kv = argv[++i];
      const size_t eq = kv.find('=');
      if (eq == std::string::npos || eq == 0 || eq + 1 >= kv.size()) return false;
      o->variants.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
    } else if (a == "--simulate-variant-click" && i + 1 < argc) {
      o->simulate_variant_click = std::atoi(argv[++i]);
    } else if (a == "--simulate-outline-click" && i + 1 < argc) {
      o->simulate_outline_click = std::atoi(argv[++i]);
    } else if (a == "--simulate-pick" && i + 1 < argc) {
      o->simulate_pick = argv[++i];
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
    } else if (a == "--no-compress-textures") {
      o->compress_textures = false;
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

// ---- Camera bookmarks -------------------------------------------------------
// Ctrl+1..9 stores the current view in a slot, Shift+1..9 recalls it. Slots are
// kept per scene file in a small text file (one line per slot: scene path, slot,
// then target xyz, yaw, pitch, distance) so they survive restarts.
struct CameraBookmark {
  bool set = false;
  float v[6] = {};  // target xyz, yaw, pitch, distance
};

std::string BookmarkFilePath() {
  if (const char* env = std::getenv("LD3D12VIEW_BOOKMARKS"); env && *env) return env;
  char base[MAX_PATH] = {};
  const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof(base));
  if (n == 0 || n >= sizeof(base)) return std::string();
  const std::string dir = std::string(base) + "\\ld3d12view";
  CreateDirectoryA(dir.c_str(), nullptr);
  return dir + "\\bookmarks.txt";
}

void LoadBookmarks(const std::string& scene, CameraBookmark out[10]) {
  for (int i = 0; i < 10; ++i) out[i] = CameraBookmark();
  const std::string file = BookmarkFilePath();
  if (file.empty()) return;
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line)) {
    const size_t t1 = line.find('\t');
    const size_t t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
    if (t2 == std::string::npos || line.compare(0, t1, scene) != 0) continue;
    const int slot = std::atoi(line.substr(t1 + 1, t2 - t1 - 1).c_str());
    CameraBookmark b;
    std::istringstream values(line.substr(t2 + 1));
    if (slot < 1 || slot > 9 ||
        !(values >> b.v[0] >> b.v[1] >> b.v[2] >> b.v[3] >> b.v[4] >> b.v[5]))
      continue;
    b.set = true;
    out[slot] = b;
  }
}

void SaveBookmarks(const std::string& scene, const CameraBookmark in[10]) {
  const std::string file = BookmarkFilePath();
  if (file.empty() || scene.empty()) return;
  std::vector<std::string> keep;  // other scenes' lines
  {
    std::ifstream old(file);
    std::string line;
    while (std::getline(old, line)) {
      const size_t t1 = line.find('\t');
      if (t1 != std::string::npos && line.compare(0, t1, scene) != 0) keep.push_back(line);
    }
  }
  std::ofstream out(file, std::ios::trunc);
  for (const std::string& line : keep) out << line << '\n';
  for (int slot = 1; slot <= 9; ++slot) {
    if (!in[slot].set) continue;
    out << scene << '\t' << slot << '\t';
    for (int i = 0; i < 6; ++i) out << in[slot].v[i] << (i < 5 ? ' ' : '\n');
  }
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
    scene_gpu.compress_textures = opt.compress_textures;
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
      int authored_index = -1;  // which authored camera is active (-1 = free view)
      float free_target[3] = {0, 0, 0};  // the free view, restored when C wraps
      float free_yaw = 0, free_pitch = 0, free_dist = 1;
      bool have_free_view = false;
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
          scene_gpu.compress_textures = opt.compress_textures;
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
        const uint32_t idx = authored_index < 0
                                 ? 0u
                                 : std::min<uint32_t>(static_cast<uint32_t>(authored_index),
                                                      camera_count - 1);
        if (lmv_scene_camera(sv.scene, idx, &cv) == LMV_OK)
          lmv_camera_set_from_camera(camera, &cv);
      };
      auto leave_authored_camera = [&] {
        use_authored = false;
        authored_index = -1;
      };
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
        if (use_authored && sv.scene && camera_count > 0 && info_text.find("cam ") == std::string::npos) {
          info_text += "  cam " + std::to_string(authored_index + 1) + "/" +
                       std::to_string(camera_count);
        }
      };
      update_info();

      // Installs a freshly loaded scene (main thread): uploads it first, so a
      // failed upload leaves the current scene alone. `initial` marks the
      // command-line scene, which also gets the --time/--preset/camera options.
      bool initial_pending = !opt.path.empty();
      ld3d12::PickResult selection;  // the clicked draw, shown in the inspector
      CameraBookmark bookmarks[10];  // slots 1..9 of the open scene
      std::string current_path;                  // the open file
      ld3d12::VariantOverrides active_variants;  // overrides it was loaded with
      // Outliner data for the open scene (built on first show).
      struct VariantRow {
        std::string prim_path, set_name, selection;
        std::vector<std::string> variants;
      };
      std::vector<VariantRow> variant_data;
      std::vector<int> mesh_first_draw;  // mesh id -> first draw, for highlighting
      bool outline_built = false;
      auto rebuild_outline = [&] {
        outline_built = true;
        variant_data.clear();
        std::vector<ld3d12::HudOutlineNode> nodes;
        std::vector<std::string> rows;
        mesh_first_draw.clear();
        if (sv.scene) {
          const uint32_t mesh_count = lmv_scene_mesh_count(sv.scene);
          mesh_first_draw.assign(mesh_count, -1);
          const uint32_t draws = lmv_scene_draw_count(sv.scene);
          for (uint32_t d = 0; d < draws; ++d) {
            lmv_draw_view dv = {};
            if (lmv_scene_draw(sv.scene, d, &dv) == LMV_OK && dv.mesh_id >= 0 &&
                static_cast<uint32_t>(dv.mesh_id) < mesh_count &&
                mesh_first_draw[static_cast<size_t>(dv.mesh_id)] < 0)
              mesh_first_draw[static_cast<size_t>(dv.mesh_id)] = static_cast<int>(d);
          }
          // Prim tree from the meshes' paths, ordered component-wise so every
          // parent precedes (and groups) its children.
          struct Item {
            std::vector<std::string> parts;
            int draw;
          };
          std::vector<Item> items;
          for (uint32_t m = 0; m < mesh_count; ++m) {
            lmv_mesh_view mv = {};
            if (mesh_first_draw[m] < 0 || lmv_scene_mesh(sv.scene, m, &mv) != LMV_OK ||
                !mv.prim_path || !*mv.prim_path)
              continue;
            Item it;
            it.draw = mesh_first_draw[m];
            std::string part;
            for (const char* c = mv.prim_path;; ++c) {
              if (*c == '/' || *c == '\0') {
                if (!part.empty()) it.parts.push_back(part);
                part.clear();
                if (*c == '\0') break;
              } else {
                part.push_back(*c);
              }
            }
            if (!it.parts.empty()) items.push_back(std::move(it));
          }
          std::sort(items.begin(), items.end(),
                    [](const Item& a, const Item& b) { return a.parts < b.parts; });
          std::map<std::string, int> index_by_path;
          for (const Item& item : items) {
            std::string path;
            int parent = -1;
            for (const std::string& part : item.parts) {
              path += "/" + part;
              auto found = index_by_path.find(path);
              if (found == index_by_path.end()) {
                ld3d12::HudOutlineNode n;
                n.name = part;
                n.parent = parent;
                nodes.push_back(n);
                found = index_by_path.emplace(path, static_cast<int>(nodes.size()) - 1).first;
              }
              parent = found->second;
            }
            nodes[static_cast<size_t>(parent)].draw = item.draw;
          }
          // Variant sets of the composed stage.
          const uint32_t vsets = lmv_scene_variant_set_count(sv.scene);
          for (uint32_t i = 0; i < vsets; ++i) {
            lmv_variant_set_view v = {};
            if (lmv_scene_variant_set(sv.scene, i, &v) != LMV_OK) continue;
            VariantRow r;
            r.prim_path = v.prim_path ? v.prim_path : "";
            r.set_name = v.set_name ? v.set_name : "";
            r.selection = v.selection ? v.selection : "";
            for (uint32_t k = 0; k < v.variant_count; ++k)
              r.variants.push_back(v.variants[k] ? v.variants[k] : "");
            for (const auto& ov : active_variants)
              if (ov.first == r.set_name) r.selection = ov.second;
            rows.push_back(r.set_name + " = " +
                           (r.selection.empty() ? "(none)" : r.selection) + "   " +
                           r.prim_path);
            variant_data.push_back(std::move(r));
          }
        }
        hud.SetOutline(nodes, rows);
        std::fprintf(stderr, "ld3d12view: outliner: %zu prim node(s), %zu variant set(s)\n",
                     nodes.size(), rows.size());
      };
      auto install_scene = [&](lmv_scene* scene, const std::string& path,
                               bool initial,
                               const ld3d12::VariantOverrides& variants) -> bool {
        selection = ld3d12::PickResult();  // draw indices belong to the old scene
        // Re-opening the same file (a variant switch) keeps the camera.
        const bool keep_view = !initial && sv.scene && path == current_path;
        SceneView next;
        MakeSceneView(scene, &next);
        device.WaitIdle();  // the old scene's buffers may still be in flight
        ld3d12::SceneGPU fresh;
        fresh.compress_textures = opt.compress_textures;
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
        if (scene_gpu.bc7_textures > 0) {
          std::fprintf(stderr,
                       "ld3d12view: %u texture(s) compressed to BC7, saved "
                       "%.1f MB of video memory\n",
                       scene_gpu.bc7_textures,
                       static_cast<double>(scene_gpu.bc7_saved_bytes) / 1048576.0);
        }
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
        if (!keep_view) FrameScene(camera, sv);
        if (initial && opt.preset >= 0) lmv_camera_set_preset(camera, opt.preset);
        lmv_camera_get_orbit(camera, free_target, &free_yaw, &free_pitch, &free_dist);
        have_free_view = true;
        authored_index = -1;
        if (initial && opt.authored_camera && camera_count > 0) {
          use_authored = true;
          authored_index = std::min<int>(opt.camera_index, static_cast<int>(camera_count) - 1);
          sync_authored_camera();
        }
        title_name = base_name(path);
        current_path = path;
        active_variants = variants;
        LoadBookmarks(current_path, bookmarks);
        {
          int stored = 0;
          for (int i = 1; i <= 9; ++i) stored += bookmarks[i].set ? 1 : 0;
          if (stored > 0)
            std::fprintf(stderr, "ld3d12view: %d camera bookmark(s) loaded\n", stored);
        }
        outline_built = false;
        if (hud.outline_visible()) rebuild_outline();
        update_info();
        lui_window_set_title(window, title_name.c_str());
        std::fprintf(stderr, "ld3d12view: opened %s\n", path.c_str());
        return ok;
      };
      auto request_load = [&](const std::string& path,
                              const ld3d12::VariantOverrides& variants = {}) {
        std::fprintf(stderr, "ld3d12view: loading %s\n", path.c_str());
        loader.Request(path, variants);
        update_info();
      };
      hud.on_variant_choose = [&](int row) {
        if (row < 0 || static_cast<size_t>(row) >= variant_data.size() ||
            current_path.empty())
          return;
        const VariantRow& v = variant_data[static_cast<size_t>(row)];
        if (v.variants.size() < 2) return;
        size_t cur = v.variants.size();
        for (size_t i = 0; i < v.variants.size(); ++i)
          if (v.variants[i] == v.selection) cur = i;
        const std::string next = v.variants[(cur + 1) % v.variants.size()];
        ld3d12::VariantOverrides overrides = active_variants;
        bool replaced = false;
        for (auto& ov : overrides)
          if (ov.first == v.set_name) {
            ov.second = next;
            replaced = true;
          }
        if (!replaced) overrides.emplace_back(v.set_name, next);
        std::fprintf(stderr, "ld3d12view: variant %s -> %s\n", v.set_name.c_str(),
                     next.c_str());
        request_load(current_path, overrides);
      };
      // C: step through the authored cameras, then back to the free view.
      auto cycle_camera = [&] {
        if (camera_count == 0) {
          std::fprintf(stderr, "ld3d12view: no authored cameras in this scene\n");
          return;
        }
        if (!use_authored) {
          lmv_camera_get_orbit(camera, free_target, &free_yaw, &free_pitch, &free_dist);
          have_free_view = true;
          authored_index = 0;
          use_authored = true;
        } else if (authored_index + 1 < static_cast<int>(camera_count)) {
          ++authored_index;
        } else {
          authored_index = -1;
          use_authored = false;
          if (have_free_view)
            lmv_camera_set_orbit(camera, free_target, free_yaw, free_pitch, free_dist);
        }
        if (use_authored) {
          sync_authored_camera();
          lmv_camera_view cv = {};
          const uint32_t idx = static_cast<uint32_t>(authored_index);
          const char* name = "";
          if (lmv_scene_camera(sv.scene, idx, &cv) == LMV_OK && cv.prim_path) name = cv.prim_path;
          std::fprintf(stderr, "ld3d12view: camera %d/%u %s\n", authored_index + 1,
                       camera_count, name);
        } else {
          std::fprintf(stderr, "ld3d12view: camera free view\n");
        }
        info_text.clear();
        update_info();
      };
      auto save_bookmark = [&](int slot) {
        if (slot < 1 || slot > 9) return;
        CameraBookmark& b = bookmarks[slot];
        lmv_camera_get_orbit(camera, b.v, &b.v[3], &b.v[4], &b.v[5]);
        b.set = true;
        SaveBookmarks(current_path, bookmarks);
        std::fprintf(stderr, "ld3d12view: bookmark %d saved\n", slot);
      };
      auto recall_bookmark = [&](int slot) {
        if (slot < 1 || slot > 9) return;
        const CameraBookmark& b = bookmarks[slot];
        if (!b.set) {
          std::fprintf(stderr, "ld3d12view: bookmark %d is empty\n", slot);
          return;
        }
        leave_authored_camera();
        lmv_camera_set_orbit(camera, b.v, b.v[3], b.v[4], b.v[5]);
        std::fprintf(stderr, "ld3d12view: bookmark %d recalled\n", slot);
        info_text.clear();
        update_info();
      };
      hud.on_outline_select = [&](int draw) {
        selection = ld3d12::DescribeDraw(sv.scene, draw);
        if (selection.hit)
          std::fprintf(stderr, "ld3d12view: outline select %s\n",
                       selection.prim_path.c_str());
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
      // A left press that moves less than a few pixels is a click: it picks.
      bool press_active = false;
      int press_travel = 0;
      std::vector<float> overlay;  // selection box edges (lmv_line_segment floats)
      // Casts a ray through the window point (logical pixels) into the scene.
      auto do_pick = [&](int lx, int ly) {
        int lw = 0, lh = 0;
        lui_window_get_size(window, &lw, &lh);
        if (!sv.scene || lw <= 0 || lh <= 0) return;
        const float nx = (static_cast<float>(lx) + 0.5f) / static_cast<float>(lw) * 2.0f - 1.0f;
        const float ny = 1.0f - (static_cast<float>(ly) + 0.5f) / static_cast<float>(lh) * 2.0f;
        lmv_camera_frame pcf = {};
        lmv_camera_get_frame(camera, &pcf);
        float vp[16], inv[16];
        Mul(pcf.proj, pcf.view, vp);
        if (!Invert4(vp, inv)) return;
        const float near_ndc[3] = {nx, ny, 0.0f}, far_ndc[3] = {nx, ny, 1.0f};
        float a[3], b[3];
        ld3d12::TransformPoint(inv, near_ndc, a);
        ld3d12::TransformPoint(inv, far_ndc, b);
        float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (!(len > 1e-12f)) return;
        for (float& c : d) c /= len;
        selection = ld3d12::PickScene(sv.scene, a, d);
        if (selection.hit) {
          std::fprintf(stderr,
                       "ld3d12view: picked %s (mesh %d, triangle %u, material %d)\n",
                       selection.prim_path.c_str(), selection.mesh,
                       selection.triangle, selection.material);
        } else {
          std::fprintf(stderr, "ld3d12view: pick: nothing hit\n");
        }
      };
      bool wireframe = opt.wireframe;
      bool show_grid = opt.grid;
      bool show_background = opt.background;
      bool shadows = opt.shadows;
      bool quit = !setup_ok;
      int frames = 0;
      int iteration = 0;
      int logged_phase = -1;
      if (opt.outliner) hud.SetOutlineVisible(true);
      if (!opt.path.empty()) request_load(opt.path, opt.variants);
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
                } else if (selection.hit) {
                  selection = ld3d12::PickResult();  // first Esc clears the selection
                } else {
                  quit = true;
                }
              }
              if (ev.data.key.key == 'f' || ev.data.key.key == 'F')
                FrameScene(camera, sv);
              if (ev.data.key.key == 'w' || ev.data.key.key == 'W')
                wireframe = !wireframe;
              if (ev.data.key.key >= '1' && ev.data.key.key <= '9' &&
                  (ev.data.key.mods & (LUI_MOD_CTRL | LUI_MOD_SHIFT))) {
                const int slot = ev.data.key.key - '0';
                if (ev.data.key.mods & LUI_MOD_CTRL)
                  save_bookmark(slot);
                else
                  recall_bookmark(slot);
              } else if (ev.data.key.key >= '1' && ev.data.key.key <= '7') {
                lmv_camera_set_preset(camera, ev.data.key.key - '1');
              }
              if (ev.data.key.key == 's' || ev.data.key.key == 'S')
                shadows = !shadows;
              if (ev.data.key.key == 'g' || ev.data.key.key == 'G')
                show_grid = !show_grid;
              if (ev.data.key.key == 'b' || ev.data.key.key == 'B')
                show_background = !show_background;
              if (ev.data.key.key == ' ') toggle_play();
              if (ev.data.key.key == 'h' || ev.data.key.key == 'H')
                hud.SetVisible(!hud.visible());
              if (ev.data.key.key == 'o' || ev.data.key.key == 'O') {
                hud.SetOutlineVisible(!hud.outline_visible());
                if (hud.outline_visible() && !outline_built) rebuild_outline();
              }
              if (ev.data.key.key == LUI_KEY_RIGHT && animated)
                seek(cur_time + 1.0);
              if (ev.data.key.key == LUI_KEY_LEFT && animated)
                seek(cur_time - 1.0);
              if (ev.data.key.key == LUI_KEY_HOME && animated) seek(t_start);
              if (ev.data.key.key == 'l' || ev.data.key.key == 'L')
                looping = !looping;
              if ((ev.data.key.key == 'c' || ev.data.key.key == 'C') &&
                  !(ev.data.key.mods & LUI_MOD_CTRL))
                cycle_camera();
              break;
            case LUI_EVENT_FILE_DROP: {
              // One event per dropped file; open the first, discard the rest.
              std::string first;
              while (char* dropped = lui_window_take_dropped_file(window)) {
                if (first.empty()) first = dropped;
                std::free(dropped);
              }
              if (!first.empty()) request_load(first, {});
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
              if (b == LUI_MOUSE_LEFT) {
                left = down;
                if (down) {
                  press_active = true;
                  press_travel = 0;
                } else if (press_active) {
                  press_active = false;
                  if (press_travel <= 4 && !hud_took)
                    do_pick(ev.data.mouse_button.x, ev.data.mouse_button.y);
                }
              }
              if (b == LUI_MOUSE_RIGHT) right = down;
              if (b == LUI_MOUSE_MIDDLE) middle = down;
              break;
            }
            case LUI_EVENT_MOUSE_MOVE: {
              if (hud.HandleEvent(ev)) break;
              const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
              const float dx = static_cast<float>(ev.data.mouse_move.dx);
              const float dy = static_cast<float>(ev.data.mouse_move.dy);
              if (left)
                press_travel += static_cast<int>(std::fabs(dx) + std::fabs(dy));
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
          } else if (!install_scene(lr.scene, lr.path, initial, lr.variants) && initial) {
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
        ld3d12::PointLineParams pl;
        std::memcpy(pl.view_proj, fp.view_proj, sizeof(pl.view_proj));
        std::memcpy(pl.view, cf.view, sizeof(pl.view));
        std::memcpy(pl.eye, cf.eye, sizeof(pl.eye));
        pl.pixel_scale = cf.orthographic
                             ? 0.0f
                             : 2.0f * std::tan(0.5f * cf.fov_y) /
                                   static_cast<float>(ph);
        if (sv.scene && (scene_gpu.splat_count || scene_gpu.line_count))
          points_lines.Draw(list, pl, scene_gpu);
        if (show_grid) {
          float grid_vp[16];
          Mul(fp.view_proj, sv.grid_model, grid_vp);
          grid.Draw(list, grid_vp);
        }
        // Selection: the picked draw's world-space bounding box, re-evaluated
        // each frame so it follows animation.
        if (sv.scene && selection.hit) {
          float corners[8][3];
          if (ld3d12::DrawWorldBox(sv.scene, selection.draw, corners)) {
            float centre[3] = {0, 0, 0};
            for (const auto& c : corners)
              for (int i = 0; i < 3; ++i) centre[i] += c[i] * 0.125f;
            const float dx = centre[0] - cf.eye[0], dy = centre[1] - cf.eye[1],
                        dz = centre[2] - cf.eye[2];
            const float width = 2.0f * pl.pixel_scale *
                                std::sqrt(dx * dx + dy * dy + dz * dz);  // ~2 px
            const float colour[4] = {1.0f, 0.84f, 0.2f, 1.0f};
            overlay.clear();
            ld3d12::AppendBoxEdges(corners, width, colour, &overlay);
            points_lines.DrawOverlay(list, pl, device.frame_index(), overlay);
          }
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
          hs.selection = selection.hit ? selection.lines : std::vector<std::string>();
          hud.SetOutlineSelection(
              selection.hit && selection.mesh >= 0 &&
                      static_cast<size_t>(selection.mesh) < mesh_first_draw.size()
                  ? mesh_first_draw[static_cast<size_t>(selection.mesh)]
                  : -1);
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
        if (frames == 3 &&
            (opt.simulate_variant_click >= 0 || opt.simulate_outline_click >= 0)) {
          int cx = 0, cy = 0;
          const bool ok = opt.simulate_variant_click >= 0
                              ? hud.VariantRowPoint(opt.simulate_variant_click, &cx, &cy)
                              : hud.PrimRowPoint(opt.simulate_outline_click, &cx, &cy);
          if (ok) {
            PostSyntheticClick(reinterpret_cast<HWND>(handle), cx, cy);
          } else {
            std::fprintf(stderr, "ld3d12view: no outliner row to click\n");
            exit_code = 1;
          }
        }
        if (frames == 3 && opt.simulate_camera_cycle > 0)
          for (int i = 0; i < opt.simulate_camera_cycle; ++i) cycle_camera();
        if (frames == 3 && opt.simulate_bookmark > 0) {
          float t0[3], y0, p0, d0, t1[3], y1, p1, d1;
          lmv_camera_get_orbit(camera, t0, &y0, &p0, &d0);
          save_bookmark(opt.simulate_bookmark);
          lmv_camera_orbit(camera, 60.0f, 40.0f);  // disturb the view...
          lmv_camera_dolly(camera, 2.0f);
          recall_bookmark(opt.simulate_bookmark);  // ...and bring it back
          lmv_camera_get_orbit(camera, t1, &y1, &p1, &d1);
          const float err = std::fabs(t0[0] - t1[0]) + std::fabs(t0[1] - t1[1]) +
                            std::fabs(t0[2] - t1[2]) + std::fabs(y0 - y1) +
                            std::fabs(p0 - p1) + std::fabs(d0 - d1);
          if (err < 1e-3f) {
            std::fprintf(stderr, "ld3d12view: bookmark %d round-trip ok\n",
                         opt.simulate_bookmark);
          } else {
            std::fprintf(stderr, "ld3d12view: bookmark %d round-trip mismatch (%g)\n",
                         opt.simulate_bookmark, err);
            exit_code = 1;
          }
        }
        if (frames == 3 && opt.simulate_bookmark_load > 0)
          recall_bookmark(opt.simulate_bookmark_load);
        if (frames == 3 && !opt.simulate_pick.empty()) {
          float fx = 0.5f, fy = 0.5f;
          if (std::sscanf(opt.simulate_pick.c_str(), "%f,%f", &fx, &fy) != 2) {
            std::fprintf(stderr, "ld3d12view: bad --simulate-pick value\n");
            exit_code = 1;
          } else {
            PostSyntheticClick(reinterpret_cast<HWND>(handle),
                               static_cast<int>(fx * static_cast<float>(pw)),
                               static_cast<int>(fy * static_cast<float>(ph)));
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

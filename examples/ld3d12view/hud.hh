// ld3d12view - heads-up display drawn with lightui into a CPU surface.
//
// A bottom bar (play/pause button, timeline slider, time readout) shown for
// animated scenes, and a small status panel in the top-left corner. The surface
// is 0xAARRGGBB; panels are opaque and everything else is fully transparent, so
// the GPU side can alpha-composite it over the 3D view with one constant
// opacity. Render() redraws only what changed and reports the row ranges that
// need re-uploading.
#pragma once

#include <lightui/lightui.h>

#include <functional>
#include <string>
#include <vector>

namespace ld3d12 {

struct HudStatus {
  std::string title;  // file name
  std::string info;   // "2 meshes  60 fps", ...
  bool animated = false;
  bool playing = false;
  double time = 0.0;
  double t_start = 0.0;
  double t_end = 0.0;
  double tps = 24.0;
};

// Loading card shown in the middle of the window while a scene is read.
struct HudLoading {
  bool active = false;
  bool cancelling = false;
  std::string title;    // file name
  std::string message;  // current phase, e.g. "Converting"
  float progress = 0.0f;  // 0..1
};

struct HudRows {
  int y0 = 0;  // rows [y0, y1), full width
  int y1 = 0;
};

class Hud {
 public:
  Hud() = default;
  ~Hud();
  Hud(const Hud&) = delete;
  Hud& operator=(const Hud&) = delete;

  bool Init(std::string* err);
  void Resize(int width, int height);

  void SetStatus(const HudStatus& status);
  void SetLoading(const HudLoading& loading);
  void SetVisible(bool visible);
  bool visible() const { return visible_; }

  // Pointer events only. Returns true when the HUD took the event (pointer over
  // the bar, or a drag that started on it), so the camera must ignore it.
  bool HandleEvent(const lui_event_t& event);
  bool Captures() const { return captured_; }

  // Window coordinates of the play button centre / a point on the timeline
  // (fraction 0..1); false when the bar is not shown. Used by input tests.
  bool PlayButtonPoint(int* x, int* y) const;
  bool TimelinePoint(double fraction, int* x, int* y) const;

  std::function<void()> on_toggle_play;
  std::function<void(double)> on_seek;  // seconds in scene time codes

  // Redraws dirty regions and returns the row ranges to upload.
  std::vector<HudRows> Render();
  const lvg_surface_t* surface() const { return surface_; }

  static constexpr int kBarHeight = 40;

 private:
  void LayoutBar();
  void ClearRect(int x, int y, int w, int h);
  void DrawTop();
  void DrawBar();
  void DrawLoading();
  bool BarActive() const { return visible_ && status_.animated; }
  lvg_rect_t BarRect() const;

  lvg_surface_t* surface_ = nullptr;
  lui_font_t* font_ = nullptr;
  int width_ = 0, height_ = 0;
  bool visible_ = true;

  HudStatus status_;
  std::string top_text_;
  lvg_rect_t top_rect_ = {0, 0, 0, 0};   // last drawn
  bool top_dirty_ = true;
  bool bar_dirty_ = true;
  bool bar_was_drawn_ = false;
  bool full_ = true;

  HudLoading loading_;
  lvg_rect_t loading_rect_ = {0, 0, 0, 0};  // last drawn
  bool loading_dirty_ = false;

  lui_button_t play_button_;
  lui_slider_t slider_;
  std::string time_text_;
  bool captured_ = false;
};

}  // namespace ld3d12

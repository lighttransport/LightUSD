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
  // Inspector for the picked object, one line each (empty = no selection).
  std::vector<std::string> selection;
};

// Loading card shown in the middle of the window while a scene is read.
struct HudLoading {
  bool active = false;
  bool cancelling = false;
  std::string title;    // file name
  std::string message;  // current phase, e.g. "Converting"
  float progress = 0.0f;  // 0..1
};

// One prim of the outliner tree (a path component). `draw` is the first draw of
// the prim's mesh for drawable leaves, -1 for plain groups.
struct HudOutlineNode {
  std::string name;
  int parent = -1;  // index into the node list, -1 = top level
  int draw = -1;
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

  // Outliner panel (right side): the prim tree and the stage's variant sets.
  // `variant_rows` are display strings ("look = red"); clicking one asks the
  // owner to switch to the next variant.
  void SetOutline(const std::vector<HudOutlineNode>& nodes,
                  const std::vector<std::string>& variant_rows);
  void SetOutlineVisible(bool visible);
  bool outline_visible() const { return outline_visible_; }
  void SetOutlineSelection(int draw);  // highlighted prim, -1 = none
  std::function<void(int draw)> on_outline_select;
  std::function<void(int row)> on_variant_choose;
  // Window point of a variant row / visible prim row; false when not shown.
  // Used by input tests.
  bool VariantRowPoint(int row, int* x, int* y) const;
  bool PrimRowPoint(int visible_row, int* x, int* y) const;

  // Redraws dirty regions and returns the row ranges to upload.
  std::vector<HudRows> Render();
  const lvg_surface_t* surface() const { return surface_; }

  static constexpr int kBarHeight = 40;

 private:
  void LayoutBar();
  void ClearRect(int x, int y, int w, int h);
  void DrawTop();
  void DrawSelection();
  void DrawBar();
  void DrawLoading();
  // Outliner internals (hud_outline.cc).
  bool HandleOutlineEvent(const lui_event_t& event);
  void DrawOutline();
  lvg_rect_t OutlineRect() const;
  void RebuildOutlineRows();
  int OutlineRowHeight() const;
  bool OutlineHit(int x, int y, int* kind, int* index) const;
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
  std::vector<std::string> sel_lines_;
  lvg_rect_t sel_rect_ = {0, 0, 0, 0};   // last drawn
  bool sel_dirty_ = false;
  bool bar_dirty_ = true;
  bool bar_was_drawn_ = false;
  bool full_ = true;

  // Outliner state.
  std::vector<HudOutlineNode> outline_nodes_;
  std::vector<bool> outline_expanded_;
  std::vector<int> outline_depth_;
  std::vector<bool> outline_has_children_;
  std::vector<int> outline_rows_;        // visible node indices, top to bottom
  std::vector<std::string> variant_rows_;
  bool outline_visible_ = false;
  bool outline_dirty_ = false;
  int outline_scroll_ = 0;               // first visible prim row
  int outline_selected_draw_ = -1;
  lvg_rect_t outline_rect_ = {0, 0, 0, 0};  // last drawn

  HudLoading loading_;
  lvg_rect_t loading_rect_ = {0, 0, 0, 0};  // last drawn
  bool loading_dirty_ = false;

  lui_button_t play_button_;
  lui_slider_t slider_;
  std::string time_text_;
  bool captured_ = false;
};

}  // namespace ld3d12

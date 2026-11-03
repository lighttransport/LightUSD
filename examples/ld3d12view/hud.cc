#include "hud.hh"

#include "hack_regular_ttf.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ld3d12 {
namespace {

constexpr int kFontPx = 14;
constexpr int kTimeLabelWidth = 150;
constexpr lvg_color_t kPanel = LVG_COLOR_RGB(0x1B, 0x1D, 0x23);
constexpr lvg_color_t kPanelEdge = LVG_COLOR_RGB(0x3A, 0x3F, 0x4B);
constexpr lvg_color_t kAccent = LVG_COLOR_RGB(0x58, 0x9C, 0xE0);
constexpr lvg_color_t kTrack = LVG_COLOR_RGB(0x40, 0x44, 0x4B);
constexpr lvg_color_t kText = LVG_COLOR_RGB(0xDD, 0xE1, 0xEA);
constexpr lvg_color_t kSelectionTitle = LVG_COLOR_RGB(0xFF, 0xD6, 0x5A);
constexpr lvg_color_t kTextDim = LVG_COLOR_RGB(0x9A, 0xA1, 0xB0);

}  // namespace

Hud::~Hud() {
  if (font_) lui_font_destroy(font_);
  if (surface_) lvg_surface_destroy(surface_);
}

bool Hud::Init(std::string* err) {
  font_ = lui_font_create_from_memory(lui_embedded_font_data(),
                                      lui_embedded_font_size(), kFontPx);
  if (!font_) {
    if (err) *err = "failed to create the HUD font";
    return false;
  }
  lui_button_init(&play_button_, 0, 0, 44, 26);
  lui_slider_init(&slider_, 0.0f, 1.0f, 0.0f);
  slider_.widget.width = lvg_size_fixed(100);
  slider_.thumb_radius = 7;
  slider_.on_change = [](float value, void* user) {
    auto* hud = static_cast<Hud*>(user);
    if (hud->on_seek) hud->on_seek(static_cast<double>(value));
  };
  slider_.on_change_user = this;
  return true;
}

void Hud::Resize(int width, int height) {
  if (width <= 0 || height <= 0) return;
  if (surface_ && width == width_ && height == height_) return;
  if (surface_) lvg_surface_destroy(surface_);
  surface_ = lvg_surface_create(width, height);
  width_ = width;
  height_ = height;
  full_ = true;
  LayoutBar();
}

lvg_rect_t Hud::BarRect() const {
  return lvg_rect_make(0, height_ - kBarHeight, width_, kBarHeight);
}

void Hud::LayoutBar() {
  const lvg_rect_t bar = BarRect();
  lui_button_set_rect(&play_button_, 10, bar.y + 7, 44, 26);
  const int x = 64;
  const int w = std::max(40, width_ - x - kTimeLabelWidth - 14);
  slider_.widget.computed = lvg_rect_make(x, bar.y + 8, w, 24);
}

void Hud::SetVisible(bool visible) {
  if (visible_ == visible) return;
  visible_ = visible;
  full_ = true;
  captured_ = false;
}

void Hud::SetLoading(const HudLoading& l) {
  const bool changed =
      l.active != loading_.active || l.cancelling != loading_.cancelling ||
      l.title != loading_.title || l.message != loading_.message ||
      static_cast<int>(l.progress * 400.0f) !=
          static_cast<int>(loading_.progress * 400.0f);
  loading_ = l;
  if (changed) loading_dirty_ = true;
}

void Hud::SetStatus(const HudStatus& s) {
  std::string top = s.title;
  if (!s.info.empty()) top += top.empty() ? s.info : "  " + s.info;
  if (top != top_text_) {
    top_text_ = top;
    top_dirty_ = true;
  }

  if (s.selection != sel_lines_) {
    sel_lines_ = s.selection;
    sel_dirty_ = true;
  }

  const bool bar_changed = s.animated != status_.animated ||
                           s.playing != status_.playing ||
                           s.t_start != status_.t_start ||
                           s.t_end != status_.t_end;
  status_ = s;
  slider_.min_val = static_cast<float>(s.t_start);
  slider_.max_val = static_cast<float>(std::max(s.t_end, s.t_start));
  if (bar_changed) bar_dirty_ = true;

  char text[64];
  std::snprintf(text, sizeof(text), "%.2f / %.2f", s.time, s.t_end);
  if (time_text_ != text) {
    time_text_ = text;
    bar_dirty_ = true;
  }
  if (!slider_.dragging) {
    const float before = slider_.value;
    lui_slider_set_value(&slider_, static_cast<float>(s.time));
    if (slider_.value != before) bar_dirty_ = true;
  }
}

bool Hud::PlayButtonPoint(int* x, int* y) const {
  if (!BarActive()) return false;
  *x = play_button_.rect.x + play_button_.rect.width / 2;
  *y = play_button_.rect.y + play_button_.rect.height / 2;
  return true;
}

bool Hud::TimelinePoint(double fraction, int* x, int* y) const {
  if (!BarActive()) return false;
  const lvg_rect_t r = slider_.widget.computed;
  const int left = r.x + slider_.thumb_radius;
  const int span = r.width - 2 * slider_.thumb_radius;
  *x = left + static_cast<int>(std::min(1.0, std::max(0.0, fraction)) * span);
  *y = r.y + r.height / 2;
  return true;
}

bool Hud::HandleEvent(const lui_event_t& ev) {
  if (!BarActive() || !surface_) return false;
  int x = 0, y = 0;
  bool left_button = false;
  switch (ev.type) {
    case LUI_EVENT_MOUSE_DOWN:
    case LUI_EVENT_MOUSE_UP:
      x = ev.data.mouse_button.x;
      y = ev.data.mouse_button.y;
      left_button = ev.data.mouse_button.button == LUI_MOUSE_LEFT;
      break;
    case LUI_EVENT_MOUSE_MOVE:
      x = ev.data.mouse_move.x;
      y = ev.data.mouse_move.y;
      break;
    case LUI_EVENT_SCROLL:
      x = ev.data.scroll.x;
      y = ev.data.scroll.y;
      break;
    default:
      return false;
  }
  const lvg_rect_t bar = BarRect();
  const bool inside = lvg_rect_contains_point(&bar, x, y);
  if (!inside && !captured_) return false;
  if (ev.type == LUI_EVENT_MOUSE_DOWN && inside) captured_ = true;

  const lui_button_result_t br = lui_button_handle_event(&play_button_, &ev);
  if (br & LUI_BUTTON_RESULT_REDRAW) bar_dirty_ = true;
  if ((br & LUI_BUTTON_RESULT_CLICKED) && on_toggle_play) on_toggle_play();
  if (slider_.widget.on_event && slider_.widget.on_event(&slider_.widget, &ev))
    bar_dirty_ = true;
  if (ev.type == LUI_EVENT_MOUSE_UP && left_button) {
    captured_ = false;
    bar_dirty_ = true;
  }
  return true;
}

void Hud::ClearRect(int x, int y, int w, int h) {
  if (!surface_) return;
  const int x0 = std::max(0, x), y0 = std::max(0, y);
  const int x1 = std::min(width_, x + w), y1 = std::min(height_, y + h);
  for (int row = y0; row < y1; ++row)
    std::memset(surface_->pixels + static_cast<size_t>(row) * surface_->stride + x0,
                0, static_cast<size_t>(x1 - x0) * sizeof(uint32_t));
}

void Hud::DrawTop() {
  lvg_canvas_t canvas;
  lvg_canvas_init(&canvas, surface_);
  const lvg_rect_t prev = top_rect_;
  if (prev.width > 0) ClearRect(prev.x, prev.y, prev.width, prev.height);
  top_rect_ = lvg_rect_make(0, 0, 0, 0);
  if (visible_ && !top_text_.empty()) {
    const int tw = lui_font_measure_text(font_, top_text_.c_str(), -1);
    const int lh = lui_font_line_height(font_);
    top_rect_ = lvg_rect_make(8, 8, std::min(width_ - 16, tw + 20), lh + 10);
    lvg_canvas_fill_rounded_rect(&canvas, top_rect_.x, top_rect_.y,
                                 top_rect_.width, top_rect_.height, 5, kPanel);
    lvg_canvas_stroke_rounded_rect(&canvas, top_rect_.x, top_rect_.y,
                                   top_rect_.width, top_rect_.height, 5,
                                   kPanelEdge, 1);
    lui_canvas_draw_text(&canvas, top_rect_.x + 10,
                         top_rect_.y + 5 + lui_font_ascent(font_),
                         top_text_.c_str(), -1, font_, kText);
  }
  lvg_canvas_destroy(&canvas);
}

// Inspector card under the status panel: one text line per entry; the first
// (the prim path) is drawn in the accent colour.
void Hud::DrawSelection() {
  lvg_canvas_t canvas;
  lvg_canvas_init(&canvas, surface_);
  const lvg_rect_t prev = sel_rect_;
  if (prev.width > 0) ClearRect(prev.x, prev.y, prev.width, prev.height);
  sel_rect_ = lvg_rect_make(0, 0, 0, 0);
  if (visible_ && !sel_lines_.empty()) {
    const int lh = lui_font_line_height(font_);
    int text_w = 0;
    for (const std::string& line : sel_lines_)
      text_w = std::max(text_w, lui_font_measure_text(font_, line.c_str(), -1));
    const int top_h = lh + 10;  // status panel above
    const int h = static_cast<int>(sel_lines_.size()) * (lh + 2) + 10;
    sel_rect_ = lvg_rect_make(8, 8 + top_h + 6, std::min(width_ - 16, text_w + 20), h);
    lvg_canvas_fill_rounded_rect(&canvas, sel_rect_.x, sel_rect_.y, sel_rect_.width,
                                 sel_rect_.height, 5, kPanel);
    lvg_canvas_stroke_rounded_rect(&canvas, sel_rect_.x, sel_rect_.y,
                                   sel_rect_.width, sel_rect_.height, 5, kPanelEdge, 1);
    int y = sel_rect_.y + 5 + lui_font_ascent(font_);
    for (size_t i = 0; i < sel_lines_.size(); ++i) {
      lui_canvas_draw_text(&canvas, sel_rect_.x + 10, y, sel_lines_[i].c_str(), -1,
                           font_, i == 0 ? kSelectionTitle : kText);
      y += lh + 2;
    }
  }
  lvg_canvas_destroy(&canvas);
}

void Hud::DrawLoading() {
  lvg_canvas_t canvas;
  lvg_canvas_init(&canvas, surface_);
  const lvg_rect_t prev = loading_rect_;
  if (prev.width > 0) ClearRect(prev.x, prev.y, prev.width, prev.height);
  loading_rect_ = lvg_rect_make(0, 0, 0, 0);
  if (loading_.active) {
    const int w = std::min(width_ - 24, 460);
    const int h = 100;
    const int x = (width_ - w) / 2, y = std::max(0, (height_ - h) / 2);
    loading_rect_ = lvg_rect_make(x, y, w, h);
    lvg_canvas_fill_rounded_rect(&canvas, x, y, w, h, 8, kPanel);
    lvg_canvas_stroke_rounded_rect(&canvas, x, y, w, h, 8, kPanelEdge, 1);
    const int asc = lui_font_ascent(font_);
    const std::string title = "Loading " + loading_.title;
    lui_canvas_draw_text(&canvas, x + 16, y + 12 + asc, title.c_str(), -1,
                         font_, kText);
    const std::string message =
        loading_.cancelling ? std::string("Cancelling...") : loading_.message;
    lui_canvas_draw_text(&canvas, x + 16, y + 34 + asc, message.c_str(), -1,
                         font_, kTextDim);
    const int track_x = x + 16, track_w = w - 32, track_y = y + 62;
    lvg_canvas_fill_rounded_rect(&canvas, track_x, track_y, track_w, 8, 4, kTrack);
    const float p = std::min(1.0f, std::max(0.0f, loading_.progress));
    const int fill = static_cast<int>(p * static_cast<float>(track_w));
    if (fill > 0)
      lvg_canvas_fill_rounded_rect(&canvas, track_x, track_y, std::max(fill, 8), 8,
                                   4, kAccent);
    char pct[16];
    std::snprintf(pct, sizeof(pct), "%d%%", static_cast<int>(p * 100.0f + 0.5f));
    lui_canvas_draw_text(&canvas, track_x + track_w - lui_font_measure_text(font_, pct, -1),
                         y + 78 + asc, pct, -1, font_, kText);
    lui_canvas_draw_text(&canvas, track_x, y + 78 + asc, "Esc to cancel", -1,
                         font_, kTextDim);
  }
  lvg_canvas_destroy(&canvas);
}

void Hud::DrawBar() {
  const lvg_rect_t bar = BarRect();
  lvg_canvas_t canvas;
  lvg_canvas_init(&canvas, surface_);
  ClearRect(bar.x, bar.y, bar.width, bar.height);
  bar_was_drawn_ = false;
  if (BarActive()) {
    bar_was_drawn_ = true;
    lvg_canvas_fill_rect(&canvas, bar.x, bar.y, bar.width, bar.height, kPanel);
    lvg_canvas_fill_rect(&canvas, bar.x, bar.y, bar.width, 1, kPanelEdge);

    // Play / pause button with its glyph.
    play_button_.highlighted = status_.playing;
    lui_button_draw(&canvas, &play_button_, nullptr);
    const lvg_rect_t b = play_button_.rect;
    const int cx = b.x + b.width / 2, cy = b.y + b.height / 2;
    if (status_.playing) {
      lvg_canvas_fill_rect(&canvas, cx - 6, cy - 6, 4, 12, LVG_COLOR_WHITE);
      lvg_canvas_fill_rect(&canvas, cx + 2, cy - 6, 4, 12, LVG_COLOR_WHITE);
    } else {
      lvg_canvas_fill_triangle(&canvas, cx - 5, cy - 7, cx - 5, cy + 7,
                               cx + 7, cy, LVG_COLOR_WHITE);
    }

    // Timeline and time readout.
    if (slider_.widget.draw) slider_.widget.draw(&slider_.widget, &canvas);
    const int tw = lui_font_measure_text(font_, time_text_.c_str(), -1);
    const int tx = bar.x + bar.width - 10 - std::max(tw, kTimeLabelWidth - 20);
    lui_canvas_draw_text(&canvas, tx,
                         bar.y + (bar.height - lui_font_line_height(font_)) / 2 +
                             lui_font_ascent(font_),
                         time_text_.c_str(), -1, font_, kText);
  }
  lvg_canvas_destroy(&canvas);
}

std::vector<HudRows> Hud::Render() {
  std::vector<HudRows> rows;
  if (!surface_) return rows;
  if (full_) {
    ClearRect(0, 0, width_, height_);
    top_rect_ = lvg_rect_make(0, 0, 0, 0);
    sel_rect_ = lvg_rect_make(0, 0, 0, 0);
    sel_dirty_ = true;
    bar_was_drawn_ = false;
    top_dirty_ = bar_dirty_ = true;
    loading_rect_ = lvg_rect_make(0, 0, 0, 0);
    loading_dirty_ = loading_.active;
    rows.push_back({0, height_});
    full_ = false;
    LayoutBar();
  }
  const bool whole = !rows.empty();
  if (top_dirty_) {
    const lvg_rect_t prev = top_rect_;
    DrawTop();
    top_dirty_ = false;
    if (!whole) {
      const int y0 = std::min(prev.width > 0 ? prev.y : top_rect_.y, top_rect_.y);
      const int y1 = std::max(prev.y + prev.height, top_rect_.y + top_rect_.height);
      if (y1 > y0) rows.push_back({y0, y1});
    }
  }
  if (sel_dirty_) {
    const lvg_rect_t prev = sel_rect_;
    DrawSelection();
    sel_dirty_ = false;
    if (!whole) {
      const int y0 = std::min(prev.width > 0 ? prev.y : sel_rect_.y, sel_rect_.y);
      const int y1 = std::max(prev.y + prev.height, sel_rect_.y + sel_rect_.height);
      if (y1 > y0) rows.push_back({y0, y1});
    }
  }
  if (loading_dirty_) {
    const lvg_rect_t prev = loading_rect_;
    DrawLoading();
    loading_dirty_ = false;
    if (!whole) {
      const int y0 = std::min(prev.width > 0 ? prev.y : loading_rect_.y, loading_rect_.y);
      const int y1 = std::max(prev.y + prev.height, loading_rect_.y + loading_rect_.height);
      if (y1 > y0) rows.push_back({y0, y1});
    }
  }
  if (bar_dirty_) {
    const bool had = bar_was_drawn_;
    DrawBar();
    bar_dirty_ = false;
    if (!whole && (had || bar_was_drawn_)) {
      const lvg_rect_t bar = BarRect();
      rows.push_back({bar.y, bar.y + bar.height});
    }
  }
  return rows;
}

}  // namespace ld3d12

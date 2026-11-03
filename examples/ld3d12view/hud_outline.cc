// ld3d12view - HUD outliner panel: the stage's prim tree and variant sets.
#include "hud.hh"

#include <lightui/lightui.h>

#include <algorithm>
#include <cstdio>

namespace ld3d12 {
namespace {

constexpr lvg_color_t kPanel = LVG_COLOR_RGB(0x1B, 0x1D, 0x23);
constexpr lvg_color_t kPanelEdge = LVG_COLOR_RGB(0x3A, 0x3F, 0x4B);
constexpr lvg_color_t kText = LVG_COLOR_RGB(0xDD, 0xE1, 0xEA);
constexpr lvg_color_t kTextDim = LVG_COLOR_RGB(0x9A, 0xA1, 0xB0);
constexpr lvg_color_t kRowSelected = LVG_COLOR_RGB(0x2D, 0x4F, 0x7A);
constexpr lvg_color_t kVariantText = LVG_COLOR_RGB(0xFF, 0xD6, 0x5A);
constexpr int kPad = 8;
constexpr int kIndent = 14;
constexpr int kToggleWidth = 18;

}  // namespace

int Hud::OutlineRowHeight() const { return lui_font_line_height(font_) + 4; }

lvg_rect_t Hud::OutlineRect() const {
  if (!visible_ || !outline_visible_ || width_ < 200 || height_ < 120)
    return lvg_rect_make(0, 0, 0, 0);
  const int w = std::min(340, std::max(180, width_ / 3));
  const int bottom = height_ - 8 - (BarActive() ? kBarHeight : 0);
  return lvg_rect_make(width_ - w - 8, 8, w, std::max(60, bottom - 8));
}

// Vertical layout (window y): header, [variants label + rows], prims label,
// then the scrolling prim rows. Returns the y of the first prim row and the
// number of prim rows that fit.
static void OutlineLayout(const lvg_rect_t& r, int lh, int rh, int variants,
                          int* prim_top, int* prim_fit, int* variants_top) {
  int y = r.y + kPad + lh + 6;  // below the header
  *variants_top = y;
  if (variants > 0) {
    y += lh + 4;  // "Variants" label
    *variants_top = y;
    y += variants * rh + 8;
  }
  y += lh + 4;  // "Prims" label
  *prim_top = y;
  *prim_fit = std::max(0, (r.y + r.height - kPad - y) / rh);
}

void Hud::RebuildOutlineRows() {
  outline_rows_.clear();
  const size_t n = outline_nodes_.size();
  std::vector<bool> vis(n, false);
  for (size_t i = 0; i < n; ++i) {
    const int p = outline_nodes_[i].parent;
    vis[i] = p < 0 || (vis[static_cast<size_t>(p)] && outline_expanded_[static_cast<size_t>(p)]);
    if (vis[i]) outline_rows_.push_back(static_cast<int>(i));
  }
  const lvg_rect_t r = OutlineRect();
  int top = 0, fit = 0, vtop = 0;
  OutlineLayout(r, lui_font_line_height(font_), OutlineRowHeight(),
                static_cast<int>(variant_rows_.size()), &top, &fit, &vtop);
  const int max_scroll = std::max(0, static_cast<int>(outline_rows_.size()) - fit);
  outline_scroll_ = std::min(std::max(0, outline_scroll_), max_scroll);
}

void Hud::SetOutline(const std::vector<HudOutlineNode>& nodes,
                     const std::vector<std::string>& variant_rows) {
  outline_nodes_ = nodes;
  variant_rows_ = variant_rows;
  const size_t n = nodes.size();
  outline_depth_.assign(n, 0);
  outline_has_children_.assign(n, false);
  for (size_t i = 0; i < n; ++i) {
    const int p = nodes[i].parent;
    if (p >= 0 && static_cast<size_t>(p) < i) {
      outline_depth_[i] = outline_depth_[static_cast<size_t>(p)] + 1;
      outline_has_children_[static_cast<size_t>(p)] = true;
    }
  }
  // Small scenes open fully; big ones start two levels deep.
  outline_expanded_.assign(n, false);
  for (size_t i = 0; i < n; ++i)
    outline_expanded_[i] = n <= 300 || outline_depth_[i] < 2;
  outline_scroll_ = 0;
  outline_selected_draw_ = -1;
  RebuildOutlineRows();
  outline_dirty_ = true;
}

void Hud::SetOutlineVisible(bool visible) {
  if (outline_visible_ == visible) return;
  outline_visible_ = visible;
  full_ = true;  // repaint from scratch (the panel overlaps other regions)
  RebuildOutlineRows();
}

void Hud::SetOutlineSelection(int draw) {
  if (draw == outline_selected_draw_) return;
  outline_selected_draw_ = draw;
  outline_dirty_ = true;
  if (draw < 0) return;
  // Reveal the row: expand its ancestors and scroll it into view.
  for (size_t i = 0; i < outline_nodes_.size(); ++i) {
    if (outline_nodes_[i].draw != draw) continue;
    for (int p = outline_nodes_[i].parent; p >= 0; p = outline_nodes_[static_cast<size_t>(p)].parent)
      outline_expanded_[static_cast<size_t>(p)] = true;
    RebuildOutlineRows();
    const auto it = std::find(outline_rows_.begin(), outline_rows_.end(), static_cast<int>(i));
    if (it != outline_rows_.end()) {
      const int row = static_cast<int>(it - outline_rows_.begin());
      int top = 0, fit = 0, vtop = 0;
      OutlineLayout(OutlineRect(), lui_font_line_height(font_), OutlineRowHeight(),
                    static_cast<int>(variant_rows_.size()), &top, &fit, &vtop);
      if (row < outline_scroll_) outline_scroll_ = row;
      if (fit > 0 && row >= outline_scroll_ + fit) outline_scroll_ = row - fit + 1;
      RebuildOutlineRows();
    }
    break;
  }
}

// kind 0 = variant row, 1 = prim row (index into outline_rows_).
bool Hud::OutlineHit(int x, int y, int* kind, int* index) const {
  const lvg_rect_t r = OutlineRect();
  if (r.width <= 0 || !lvg_rect_contains_point(&r, x, y)) return false;
  const int lh = lui_font_line_height(font_), rh = OutlineRowHeight();
  int prim_top = 0, fit = 0, vtop = 0;
  OutlineLayout(r, lh, rh, static_cast<int>(variant_rows_.size()), &prim_top, &fit, &vtop);
  if (!variant_rows_.empty() && y >= vtop &&
      y < vtop + static_cast<int>(variant_rows_.size()) * rh) {
    *kind = 0;
    *index = (y - vtop) / rh;
    return true;
  }
  if (y >= prim_top && y < prim_top + fit * rh) {
    const int row = outline_scroll_ + (y - prim_top) / rh;
    if (row < static_cast<int>(outline_rows_.size())) {
      *kind = 1;
      *index = row;
      return true;
    }
  }
  return false;
}

bool Hud::VariantRowPoint(int row, int* x, int* y) const {
  const lvg_rect_t r = OutlineRect();
  if (r.width <= 0 || row < 0 || row >= static_cast<int>(variant_rows_.size()))
    return false;
  int prim_top = 0, fit = 0, vtop = 0;
  OutlineLayout(r, lui_font_line_height(font_), OutlineRowHeight(),
                static_cast<int>(variant_rows_.size()), &prim_top, &fit, &vtop);
  *x = r.x + r.width / 2;
  *y = vtop + row * OutlineRowHeight() + OutlineRowHeight() / 2;
  return true;
}

bool Hud::PrimRowPoint(int visible_row, int* x, int* y) const {
  const lvg_rect_t r = OutlineRect();
  if (r.width <= 0 || visible_row < 0 ||
      visible_row >= static_cast<int>(outline_rows_.size()))
    return false;
  int prim_top = 0, fit = 0, vtop = 0;
  OutlineLayout(r, lui_font_line_height(font_), OutlineRowHeight(),
                static_cast<int>(variant_rows_.size()), &prim_top, &fit, &vtop);
  const int local = visible_row - outline_scroll_;
  if (local < 0 || local >= fit) return false;
  // Right of the toggle zone so a click selects instead of expanding.
  const int node = outline_rows_[static_cast<size_t>(visible_row)];
  *x = r.x + kPad + outline_depth_[static_cast<size_t>(node)] * kIndent + kToggleWidth + 12;
  *y = prim_top + local * OutlineRowHeight() + OutlineRowHeight() / 2;
  return true;
}

bool Hud::HandleOutlineEvent(const lui_event_t& ev) {
  if (!surface_) return false;
  const lvg_rect_t r = OutlineRect();
  if (r.width <= 0) return false;
  int x = 0, y = 0;
  switch (ev.type) {
    case LUI_EVENT_MOUSE_DOWN:
    case LUI_EVENT_MOUSE_UP:
      x = ev.data.mouse_button.x;
      y = ev.data.mouse_button.y;
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
  if (!lvg_rect_contains_point(&r, x, y)) return false;

  if (ev.type == LUI_EVENT_SCROLL) {
    outline_scroll_ += ev.data.scroll.delta_y > 0 ? 3 : -3;
    RebuildOutlineRows();  // clamps
    outline_dirty_ = true;
  } else if (ev.type == LUI_EVENT_MOUSE_DOWN &&
             ev.data.mouse_button.button == LUI_MOUSE_LEFT) {
    int kind = 0, index = 0;
    if (OutlineHit(x, y, &kind, &index)) {
      if (kind == 0) {
        if (on_variant_choose) on_variant_choose(index);
      } else {
        const size_t node = static_cast<size_t>(outline_rows_[static_cast<size_t>(index)]);
        const int indent = r.x + kPad + outline_depth_[node] * kIndent;
        const bool in_toggle = x < indent + kToggleWidth;
        const HudOutlineNode& n = outline_nodes_[node];
        if (outline_has_children_[node] && (in_toggle || n.draw < 0)) {
          outline_expanded_[node] = !outline_expanded_[node];
          RebuildOutlineRows();
          outline_dirty_ = true;
        } else if (n.draw >= 0) {
          outline_selected_draw_ = n.draw;
          outline_dirty_ = true;
          if (on_outline_select) on_outline_select(n.draw);
        }
      }
    }
  }
  return true;  // inside the panel: the camera must ignore it
}

void Hud::DrawOutline() {
  lvg_canvas_t canvas;
  lvg_canvas_init(&canvas, surface_);
  const lvg_rect_t prev = outline_rect_;
  if (prev.width > 0) ClearRect(prev.x, prev.y, prev.width, prev.height);
  outline_rect_ = OutlineRect();
  const lvg_rect_t r = outline_rect_;
  if (r.width > 0) {
    const int lh = lui_font_line_height(font_), rh = OutlineRowHeight();
    lvg_canvas_fill_rounded_rect(&canvas, r.x, r.y, r.width, r.height, 6, kPanel);
    lvg_canvas_stroke_rounded_rect(&canvas, r.x, r.y, r.width, r.height, 6, kPanelEdge, 1);
    lvg_canvas_set_clip(&canvas, &r);
    const int asc = lui_font_ascent(font_);
    int y = r.y + kPad;
    lui_canvas_draw_text(&canvas, r.x + kPad + 2, y + asc, "Outliner", -1, font_, kText);
    int prim_top = 0, fit = 0, vtop = 0;
    OutlineLayout(r, lh, rh, static_cast<int>(variant_rows_.size()), &prim_top, &fit, &vtop);
    if (!variant_rows_.empty()) {
      lui_canvas_draw_text(&canvas, r.x + kPad + 2, vtop - 4 - lh + asc, "Variants (click to change)", -1,
                           font_, kTextDim);
      for (size_t i = 0; i < variant_rows_.size(); ++i)
        lui_canvas_draw_text(&canvas, r.x + kPad + 6, vtop + static_cast<int>(i) * rh + 2 + asc,
                             variant_rows_[i].c_str(), -1, font_, kVariantText);
    }
    lui_canvas_draw_text(&canvas, r.x + kPad + 2, prim_top - 4 - lh + asc, "Prims", -1, font_,
                         kTextDim);
    for (int i = 0; i < fit; ++i) {
      const int row = outline_scroll_ + i;
      if (row >= static_cast<int>(outline_rows_.size())) break;
      const size_t node = static_cast<size_t>(outline_rows_[static_cast<size_t>(row)]);
      const HudOutlineNode& n = outline_nodes_[node];
      const int ry = prim_top + i * rh;
      if (n.draw >= 0 && n.draw == outline_selected_draw_)
        lvg_canvas_fill_rect(&canvas, r.x + 4, ry, r.width - 8, rh, kRowSelected);
      const int x = r.x + kPad + outline_depth_[node] * kIndent;
      if (outline_has_children_[node])
        lui_canvas_draw_text(&canvas, x + 2, ry + 2 + asc,
                             outline_expanded_[node] ? "-" : "+", -1, font_, kTextDim);
      lui_canvas_draw_text(&canvas, x + kToggleWidth, ry + 2 + asc, n.name.c_str(), -1, font_,
                           n.draw >= 0 ? kText : kTextDim);
    }
    // Scroll position hint when the list is longer than the panel.
    const int total = static_cast<int>(outline_rows_.size());
    if (total > fit && fit > 0) {
      const int track_h = fit * rh;
      const int thumb_h = std::max(12, track_h * fit / total);
      const int thumb_y = prim_top + (track_h - thumb_h) * outline_scroll_ / std::max(1, total - fit);
      lvg_canvas_fill_rect(&canvas, r.x + r.width - 6, thumb_y, 3, thumb_h, kPanelEdge);
    }
    lvg_canvas_reset_clip(&canvas);
  }
  lvg_canvas_destroy(&canvas);
}

}  // namespace ld3d12

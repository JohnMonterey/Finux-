#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/geometry.hpp"
#include "gfx/canvas.hpp"
#include "text/font.hpp"
#include "text/text_renderer.hpp"
#include "ui/theme.hpp"

namespace finux::ui {

class Widget;

// Everything a widget needs in order to draw itself. `origin` is the widget's
// top-left in surface coordinates, so widgets can lay out in local space and
// convert only at the point of drawing.
struct PaintContext {
  gfx::Canvas& canvas;
  text::TextRenderer& text;
  const Theme& theme;
  text::Font& uiFont;
  Point origin;

  Rect toSurface(Rect local) const {
    return local.translated(origin.x, origin.y);
  }
  Point toSurface(Point local) const {
    return {local.x + origin.x, local.y + origin.y};
  }

  void fill(Rect local, Color c) { canvas.fillRect(toSurface(local), c); }
  void label(Rect local, std::string_view s, Color c,
             text::HAlign h = text::HAlign::Left,
             text::VAlign v = text::VAlign::Middle) {
    // The glyph blitter writes to the surface directly, so the paint context
    // must be stood down first; it revives on the next Canvas call.
    canvas.syncPixels();
    text.drawInBox(canvas.target(), toSurface(local), uiFont, s, c, h, v);
  }
};

enum class MouseButton { Left, Middle, Right };

struct MouseEvent {
  Point position;  // in the receiving widget's local coordinates
  MouseButton button = MouseButton::Left;
};

// Retained widget tree.
//
// Intentionally minimal: the shell needs a parent/child hierarchy, hit
// testing, hover/press state and a repaint signal, and nothing else. Anything
// richer would start making layout decisions, which is precisely the authority
// that has to stay with the Windows metrics in Theme.
class Widget {
 public:
  Widget() = default;
  virtual ~Widget() = default;
  Widget(const Widget&) = delete;
  Widget& operator=(const Widget&) = delete;

  // --- Tree ----------------------------------------------------------------
  Widget* addChild(std::unique_ptr<Widget> child);
  const std::vector<std::unique_ptr<Widget>>& children() const { return children_; }
  void clearChildren();
  Widget* parent() const { return parent_; }

  // --- Geometry ------------------------------------------------------------
  // Bounds are in the parent's coordinate space.
  void setBounds(Rect bounds);
  Rect bounds() const { return bounds_; }
  Rect localBounds() const { return {0, 0, bounds_.w, bounds_.h}; }

  void setVisible(bool visible);
  bool visible() const { return visible_; }

  // --- State ---------------------------------------------------------------
  bool hovered() const { return hovered_; }
  bool pressed() const { return pressed_; }

  // --- Painting ------------------------------------------------------------
  // Walks the subtree, clipping and translating per widget.
  void paintTree(PaintContext& ctx);

  // Marks this widget dirty. Propagates to the root, which is where a
  // repaint is actually scheduled.
  void invalidate();
  bool dirty() const { return dirty_; }
  void clearDirty() { dirty_ = false; }

  // --- Input ---------------------------------------------------------------
  // Deepest visible child containing `point` (given in this widget's local
  // space), or this widget. Never returns null.
  Widget* hitTest(Point point);

  // Dispatch helpers used by the shell's event loop. `point` is local to this
  // widget. Returns the widget that handled the event, if any.
  Widget* dispatchMouseMove(Point point);
  Widget* dispatchMouseDown(Point point, MouseButton button);
  Widget* dispatchMouseUp(Point point, MouseButton button);
  void dispatchMouseLeave();

 protected:
  virtual void onPaint(PaintContext& ctx) { (void)ctx; }
  virtual void onLayout() {}
  virtual void onMouseEnter() {}
  virtual void onMouseLeave() {}
  virtual void onMouseMove(const MouseEvent&) {}
  virtual void onMouseDown(const MouseEvent&) {}
  virtual void onMouseUp(const MouseEvent&) {}

  // True if this widget accepts pointer input. Decorative widgets return false
  // so hit testing falls through to whatever is behind them.
  virtual bool interactive() const { return false; }

 private:
  void setHovered(bool hovered);

  Widget* parent_ = nullptr;
  std::vector<std::unique_ptr<Widget>> children_;
  Rect bounds_;
  bool visible_ = true;
  bool hovered_ = false;
  bool pressed_ = false;
  bool dirty_ = true;
  Widget* hoveredChild_ = nullptr;
};

}  // namespace finux::ui

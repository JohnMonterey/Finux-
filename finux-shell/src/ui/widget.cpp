#include "ui/widget.hpp"

namespace finux::ui {

Widget* Widget::addChild(std::unique_ptr<Widget> child) {
  if (!child) return nullptr;
  child->parent_ = this;
  Widget* raw = child.get();
  children_.push_back(std::move(child));
  invalidate();
  return raw;
}

void Widget::clearChildren() {
  hoveredChild_ = nullptr;
  children_.clear();
  invalidate();
}

void Widget::setBounds(Rect bounds) {
  if (bounds_ == bounds) return;
  const bool resized = bounds_.size() != bounds.size();
  bounds_ = bounds;
  if (resized) onLayout();
  invalidate();
}

void Widget::setVisible(bool visible) {
  if (visible_ == visible) return;
  visible_ = visible;
  if (!visible_) {
    setHovered(false);
    pressed_ = false;
  }
  invalidate();
}

void Widget::invalidate() {
  dirty_ = true;
  for (Widget* w = parent_; w; w = w->parent_) w->dirty_ = true;
}

void Widget::paintTree(PaintContext& ctx) {
  if (!visible_ || bounds_.empty()) return;

  const Rect surfaceRect = ctx.toSurface(bounds_);
  ctx.canvas.pushClip(surfaceRect);
  if (!ctx.canvas.clip().empty()) {
    PaintContext local = ctx;
    local.origin = {surfaceRect.x, surfaceRect.y};
    onPaint(local);
    for (const std::unique_ptr<Widget>& child : children_) {
      child->paintTree(local);
    }
  }
  ctx.canvas.popClip();
  dirty_ = false;
}

Widget* Widget::hitTest(Point point) {
  if (!visible_ || !localBounds().contains(point)) return nullptr;
  // Reverse order: later children paint on top, so they get input first.
  for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
    Widget* child = it->get();
    const Point childPoint{point.x - child->bounds_.x, point.y - child->bounds_.y};
    if (Widget* hit = child->hitTest(childPoint)) return hit;
  }
  return interactive() ? this : nullptr;
}

void Widget::setHovered(bool hovered) {
  if (hovered_ == hovered) return;
  hovered_ = hovered;
  if (hovered_) {
    onMouseEnter();
  } else {
    pressed_ = false;
    onMouseLeave();
  }
  invalidate();
}

Widget* Widget::dispatchMouseMove(Point point) {
  Widget* target = hitTest(point);

  // Enter/leave is tracked against the previously hovered widget rather than
  // recomputed per frame, so a widget that moves under a stationary pointer
  // still gets a consistent enter/leave pair.
  if (hoveredChild_ != target) {
    if (hoveredChild_) hoveredChild_->setHovered(false);
    hoveredChild_ = target;
    if (hoveredChild_) hoveredChild_->setHovered(true);
  }

  if (!target) return nullptr;

  MouseEvent event;
  event.position = point;
  for (Widget* w = target; w && w != this; w = w->parent_) {
    event.position.x -= w->bounds_.x;
    event.position.y -= w->bounds_.y;
  }
  target->onMouseMove(event);
  return target;
}

Widget* Widget::dispatchMouseDown(Point point, MouseButton button) {
  Widget* target = hitTest(point);
  if (!target) return nullptr;

  MouseEvent event;
  event.position = point;
  for (Widget* w = target; w && w != this; w = w->parent_) {
    event.position.x -= w->bounds_.x;
    event.position.y -= w->bounds_.y;
  }
  event.button = button;
  target->pressed_ = true;
  target->invalidate();
  target->onMouseDown(event);
  return target;
}

Widget* Widget::dispatchMouseUp(Point point, MouseButton button) {
  Widget* target = hitTest(point);
  if (!target) return nullptr;

  MouseEvent event;
  event.position = point;
  for (Widget* w = target; w && w != this; w = w->parent_) {
    event.position.x -= w->bounds_.x;
    event.position.y -= w->bounds_.y;
  }
  event.button = button;
  target->pressed_ = false;
  target->invalidate();
  target->onMouseUp(event);
  return target;
}

void Widget::dispatchMouseLeave() {
  if (hoveredChild_) {
    hoveredChild_->setHovered(false);
    hoveredChild_ = nullptr;
  }
}

}  // namespace finux::ui

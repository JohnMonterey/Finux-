#pragma once

#include <algorithm>
#include <cstdint>

namespace finux {

// Integer geometry throughout. Windows 10's shell is laid out on whole device
// pixels at every scale factor; carrying floats here is how 1px seams appear.
struct Point {
  int x = 0;
  int y = 0;
  friend constexpr bool operator==(Point, Point) = default;
};

struct Size {
  int w = 0;
  int h = 0;
  constexpr bool empty() const { return w <= 0 || h <= 0; }
  friend constexpr bool operator==(Size, Size) = default;
};

struct Rect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;

  constexpr int left() const { return x; }
  constexpr int top() const { return y; }
  constexpr int right() const { return x + w; }
  constexpr int bottom() const { return y + h; }
  constexpr Size size() const { return {w, h}; }
  constexpr Point origin() const { return {x, y}; }
  constexpr bool empty() const { return w <= 0 || h <= 0; }

  constexpr bool contains(Point p) const {
    return p.x >= x && p.y >= y && p.x < right() && p.y < bottom();
  }

  constexpr Rect translated(int dx, int dy) const { return {x + dx, y + dy, w, h}; }

  constexpr Rect inset(int d) const { return inset(d, d, d, d); }
  constexpr Rect inset(int l, int t, int r, int b) const {
    return {x + l, y + t, w - l - r, h - t - b};
  }

  constexpr Rect intersected(Rect o) const {
    const int nx = std::max(x, o.x);
    const int ny = std::max(y, o.y);
    const int nr = std::min(right(), o.right());
    const int nb = std::min(bottom(), o.bottom());
    if (nr <= nx || nb <= ny) return {};
    return {nx, ny, nr - nx, nb - ny};
  }

  constexpr bool intersects(Rect o) const { return !intersected(o).empty(); }

  constexpr Rect united(Rect o) const {
    if (empty()) return o;
    if (o.empty()) return *this;
    const int nx = std::min(x, o.x);
    const int ny = std::min(y, o.y);
    return {nx, ny, std::max(right(), o.right()) - nx,
            std::max(bottom(), o.bottom()) - ny};
  }

  friend constexpr bool operator==(Rect, Rect) = default;
};

// Edge insets, ordered as CSS does: left/top/right/bottom.
struct Insets {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  static constexpr Insets all(int v) { return {v, v, v, v}; }
  static constexpr Insets symmetric(int h, int v) { return {h, v, h, v}; }
  constexpr int horizontal() const { return left + right; }
  constexpr int vertical() const { return top + bottom; }
};

}  // namespace finux

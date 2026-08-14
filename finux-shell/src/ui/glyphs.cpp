#include "ui/glyphs.hpp"

#include <algorithm>
#include <cmath>

namespace finux::ui::glyphs {
namespace {

// Shrinks `box` to a centred square, so a glyph handed a non-square layout
// rect still draws round.
Rect square(Rect box) {
  const int side = std::min(box.w, box.h);
  return {box.x + (box.w - side) / 2, box.y + (box.h - side) / 2, side, side};
}

Point centerOf(Rect r) { return {r.x + r.w / 2, r.y + r.h / 2}; }

// Stroke weight that keeps glyphs looking like one family across sizes.
double weightFor(Rect box) {
  return std::max(1.0, box.w / 12.0);
}

}  // namespace

void windowsLogo(gfx::Canvas& canvas, Rect box, Color color) {
  // Four panes with a gap, in the flat post-8 style. This is a generic
  // four-pane mark, not Microsoft's logo, which is a trademark.
  const Rect s = square(box);
  const int gap = std::max(1, s.w / 9);
  const int pane = (s.w - gap) / 2;
  canvas.fillRect({s.x, s.y, pane, pane}, color);
  canvas.fillRect({s.x + pane + gap, s.y, pane, pane}, color);
  canvas.fillRect({s.x, s.y + pane + gap, pane, pane}, color);
  canvas.fillRect({s.x + pane + gap, s.y + pane + gap, pane, pane}, color);
}

void hamburger(gfx::Canvas& canvas, Rect box, Color color) {
  const Rect s = square(box);
  const int thickness = std::max(1, s.h / 12);
  const int spacing = s.h / 4;
  const int y = s.y + (s.h - spacing * 2 - thickness) / 2;
  for (int i = 0; i < 3; ++i) {
    canvas.fillRect({s.x, y + i * spacing, s.w, thickness}, color);
  }
}

void search(gfx::Canvas& canvas, Rect box, Color color) {
  const Rect s = square(box);
  const double weight = weightFor(s);
  const int radius = s.w * 5 / 16;
  const Point center{s.x + radius + 1, s.y + radius + 1};
  canvas.strokeCircle(center, radius, weight, color);
  canvas.strokeLine({center.x + radius * 3 / 4, center.y + radius * 3 / 4},
                    {s.right() - 1, s.bottom() - 1}, weight, color);
}

void cortanaRing(gfx::Canvas& canvas, Rect box, Color color) {
  const Rect s = square(box);
  canvas.strokeCircle(centerOf(s), s.w / 2 - 1, weightFor(s), color);
}

void taskView(gfx::Canvas& canvas, Rect box, Color color) {
  // A large pane with two smaller ones behind it, the shape Windows uses for
  // "show all open windows".
  const Rect s = square(box);
  const double weight = std::max(1.0, s.w / 16.0);
  const int mainHeight = s.h * 5 / 8;
  const Rect main{s.x, s.y + (s.h - mainHeight) / 2 + 2, s.w * 5 / 8,
                  mainHeight};
  canvas.strokeRectInside(main, color, static_cast<int>(weight));

  const int sideWidth = s.w / 5;
  const int sideHeight = mainHeight * 3 / 4;
  const int sideY = main.y + (mainHeight - sideHeight) / 2;
  canvas.fillRect({main.right() + 2, sideY, sideWidth, sideHeight}, color);
}

void user(gfx::Canvas& canvas, Rect box, Color color) {
  const Rect s = square(box);
  const double weight = weightFor(s);
  const int headRadius = s.w / 5;
  const Point head{s.x + s.w / 2, s.y + headRadius + 2};
  canvas.strokeCircle(head, headRadius, weight, color);
  // Shoulders: an arc opening downward beneath the head.
  canvas.strokeArc({s.x + s.w / 2, s.bottom() - 1}, s.w * 2 / 5, 180.0, 180.0,
                   weight, color);
}

void document(gfx::Canvas& canvas, Rect box, Color color) {
  const Rect s = square(box);
  const double weight = std::max(1.0, s.w / 14.0);
  const int inset = s.w / 6;
  const Rect page{s.x + inset, s.y + 1, s.w - inset * 2, s.h - 2};
  canvas.strokeRectInside(page, color, static_cast<int>(weight));
  // Text lines.
  const int lineInset = page.w / 5;
  for (int i = 1; i <= 3; ++i) {
    const int y = page.y + page.h * i / 4;
    canvas.fillRect({page.x + lineInset, y, page.w - lineInset * 2, 1}, color);
  }
}

void picture(gfx::Canvas& canvas, Rect box, Color color) {
  const Rect s = square(box);
  const double weight = std::max(1.0, s.w / 14.0);
  const int frameHeight = s.h * 3 / 4;
  const Rect frame{s.x, s.y + (s.h - frameHeight) / 2, s.w, frameHeight};
  canvas.strokeRectInside(frame, color, static_cast<int>(weight));
  // A sun and a hill, the universal "this is an image" shorthand.
  canvas.fillCircle({frame.x + frame.w / 4, frame.y + frame.h / 3},
                    std::max(1, frame.w / 10), color);
  canvas.fillTriangle({frame.x + 2, frame.bottom() - 2},
                      {frame.x + frame.w / 2, frame.y + frame.h / 3},
                      {frame.right() - 2, frame.bottom() - 2}, color);
}

void settings(gfx::Canvas& canvas, Rect box, Color color) {
  // A ring with radial teeth. Not a literal gear outline, but at 20px the
  // silhouette is what registers, and this keeps the stroke weight uniform.
  const Rect s = square(box);
  const Point center = centerOf(s);
  const double weight = weightFor(s);
  const int outer = s.w / 2 - 2;
  canvas.strokeCircle(center, outer * 3 / 5, weight, color);

  constexpr double kPi = 3.14159265358979323846;
  for (int i = 0; i < 8; ++i) {
    const double angle = i * kPi / 4.0;
    const auto x0 = static_cast<int>(center.x + std::cos(angle) * outer * 0.62);
    const auto y0 = static_cast<int>(center.y + std::sin(angle) * outer * 0.62);
    const auto x1 = static_cast<int>(center.x + std::cos(angle) * outer);
    const auto y1 = static_cast<int>(center.y + std::sin(angle) * outer);
    canvas.strokeLine({x0, y0}, {x1, y1}, weight, color);
  }
}

void power(gfx::Canvas& canvas, Rect box, Color color) {
  const Rect s = square(box);
  const Point center = centerOf(s);
  const double weight = weightFor(s);
  const int radius = s.w / 2 - 2;
  // Ring broken at the top, with a vertical bar through the gap.
  canvas.strokeArc(center, radius, -60.0, 300.0, weight, color);
  canvas.strokeLine({center.x, s.y + 1}, {center.x, center.y - radius / 3},
                    weight, color);
}

void chevronUp(gfx::Canvas& canvas, Rect box, Color color) {
  const Rect s = square(box);
  const double weight = std::max(1.0, s.w / 14.0);
  const int half = s.w / 4;
  const Point apex{s.x + s.w / 2, s.y + s.h / 2 - half / 2};
  canvas.strokeLine({apex.x - half, apex.y + half}, apex, weight, color);
  canvas.strokeLine(apex, {apex.x + half, apex.y + half}, weight, color);
}

void cloud(gfx::Canvas& canvas, Rect box, Color color) {
  const Rect s = square(box);
  const double weight = std::max(1.0, s.w / 14.0);
  const int y = s.y + s.h * 5 / 8;
  canvas.strokeArc({s.x + s.w * 3 / 8, y}, s.w / 4, 180.0, 180.0, weight, color);
  canvas.strokeArc({s.x + s.w * 5 / 8, y}, s.w / 5, 200.0, 160.0, weight, color);
  canvas.strokeLine({s.x + s.w / 8, y}, {s.right() - s.w / 8, y}, weight, color);
}

void network(gfx::Canvas& canvas, Rect box, Color color) {
  // Concentric arcs over a dot: the wireless-strength mark.
  const Rect s = square(box);
  const double weight = std::max(1.0, s.w / 14.0);
  const Point origin{s.x + s.w / 2, s.bottom() - 2};
  canvas.fillCircle(origin, std::max(1, s.w / 12), color);
  for (int i = 1; i <= 3; ++i) {
    canvas.strokeArc(origin, s.w * i / 7, 215.0, 110.0, weight, color);
  }
}

void volume(gfx::Canvas& canvas, Rect box, Color color) {
  const Rect s = square(box);
  const double weight = std::max(1.0, s.w / 14.0);
  // Speaker body plus cone.
  const int bodyHeight = s.h / 3;
  canvas.fillRect({s.x + s.w / 8, s.y + (s.h - bodyHeight) / 2, s.w / 5,
                   bodyHeight},
                  color);
  canvas.fillTriangle({s.x + s.w * 2 / 5, s.y + s.h / 2},
                      {s.x + s.w * 3 / 5, s.y + s.h / 5},
                      {s.x + s.w * 3 / 5, s.bottom() - s.h / 5}, color);
  // Two waves.
  const Point origin{s.x + s.w * 3 / 5, s.y + s.h / 2};
  canvas.strokeArc(origin, s.w / 5, -50.0, 100.0, weight, color);
  canvas.strokeArc(origin, s.w / 3, -50.0, 100.0, weight, color);
}

void actionCenter(gfx::Canvas& canvas, Rect box, Color color) {
  // A speech balloon: rounded body with a tail at the lower left.
  const Rect s = square(box);
  const double weight = std::max(1.0, s.w / 14.0);
  const int bodyHeight = s.h * 3 / 4;
  const Rect body{s.x, s.y + 1, s.w, bodyHeight};
  canvas.strokeRectInside(body, color, static_cast<int>(weight));
  canvas.strokeLine({body.x + 2, body.bottom()},
                    {body.x + 2, s.bottom() - 1}, weight, color);
  canvas.strokeLine({body.x + 2, s.bottom() - 1},
                    {body.x + body.w / 4, body.bottom()}, weight, color);
}

void appPlaceholder(gfx::Canvas& canvas, Rect box, Color color) {
  canvas.fillRect(box, color);
}

}  // namespace finux::ui::glyphs

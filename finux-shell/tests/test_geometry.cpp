#include "check.hpp"
#include "core/color.hpp"
#include "core/geometry.hpp"
#include "ui/theme.hpp"

using namespace finux;

namespace {

void rectBasics() {
  const Rect r{10, 20, 100, 40};
  CHECK_EQ(r.right(), 110);
  CHECK_EQ(r.bottom(), 60);
  CHECK(r.contains({10, 20}));
  CHECK(!r.contains({110, 60}));  // right/bottom edges are exclusive
  CHECK(!r.contains({9, 20}));
}

void intersection() {
  const Rect a{0, 0, 100, 40};
  const Rect b{50, 10, 100, 40};
  CHECK(a.intersected(b) == Rect(50, 10, 50, 30));
  CHECK(a.intersects(b));

  // Touching but not overlapping must not count as an intersection, or
  // adjacent taskbar buttons would each claim the shared edge pixel.
  const Rect c{100, 0, 10, 40};
  CHECK(!a.intersects(c));
  CHECK(a.intersected(c).empty());
}

void insets() {
  const Rect r{0, 0, 100, 40};
  CHECK(r.inset(4) == Rect(4, 4, 92, 32));
  CHECK(r.inset(0, 0, 8, 0) == Rect(0, 0, 92, 40));
}

void colorPacking() {
  const Color accent = Color::rgb(0x0078D7);
  CHECK_EQ(static_cast<int>(accent.r), 0x00);
  CHECK_EQ(static_cast<int>(accent.g), 0x78);
  CHECK_EQ(static_cast<int>(accent.b), 0xD7);
  CHECK_EQ(static_cast<int>(accent.a), 0xFF);
  CHECK(accent.argb() == 0xFF0078D7u);
}

// Translucent shell layers are stored straight and flattened on demand; the
// two must agree, since pixdiff references are captured as opaque pixels.
void flattening() {
  const Color half = Color::rgba(0xFFFFFF, 0x80);
  const Color over = flatten(half, Color::rgb(0x000000));
  CHECK_EQ(static_cast<int>(over.a), 255);
  CHECK(over.r == over.g && over.g == over.b);
  CHECK(over.r >= 127 && over.r <= 129);

  // Fully transparent leaves the backdrop untouched.
  CHECK(flatten(Color::rgba(0xFF0000, 0), Color::rgb(0x1F1F1F)) ==
        Color::rgb(0x1F1F1F));
}

void themeScaling() {
  ui::Theme theme = ui::Theme::win10();
  CHECK_EQ(theme.taskbarHeight(), 40);
  CHECK_EQ(theme.taskbarHeight(true), 30);

  // Windows' scale steps must land on whole pixels.
  theme.scalePercent = 150;
  CHECK_EQ(theme.taskbarHeight(), 60);
  CHECK_EQ(theme.scaled(theme.metrics.uiFontPixelSize), 18);

  theme.scalePercent = 125;
  CHECK_EQ(theme.taskbarHeight(), 50);
  CHECK_EQ(theme.scaled(theme.metrics.uiFontPixelSize), 15);

  // 175% is the awkward one: rounding must not drift.
  theme.scalePercent = 175;
  CHECK_EQ(theme.taskbarHeight(), 70);
}

}  // namespace

int main() {
  rectBasics();
  intersection();
  insets();
  colorPacking();
  flattening();
  themeScaling();
  return finux::check::finish("geometry");
}

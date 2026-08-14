// Tests the comparator itself. If the fidelity harness is wrong, every
// fidelity result built on it is worthless, so it gets tested first.

#include "check.hpp"
#include "core/color.hpp"
#include "gfx/canvas.hpp"
#include "gfx/image.hpp"
#include "pixdiff/pixdiff.hpp"

using namespace finux;

namespace {

gfx::Image solid(int w, int h, Color color) {
  gfx::Image image(w, h);
  image.fill(color);
  return image;
}

void identicalImagesPass() {
  const gfx::Image a = solid(32, 16, Color::rgb(0x1F1F1F));
  const gfx::Image b = solid(32, 16, Color::rgb(0x1F1F1F));
  const pixdiff::Result r = pixdiff::compare(a, b);
  CHECK(r.sizesMatch);
  CHECK_MSG(r.passed, r.message);
  CHECK(r.failingPixels == 0);
  CHECK(r.maxDelta < 1e-9);
}

void sizeMismatchIsNotAPass() {
  const gfx::Image a = solid(32, 16, Color::rgb(0x000000));
  const gfx::Image b = solid(32, 17, Color::rgb(0x000000));
  const pixdiff::Result r = pixdiff::compare(a, b);
  CHECK(!r.sizesMatch);
  CHECK(!r.passed);
}

// The point of comparing in Lab: a small RGB step in the near-black range the
// taskbar occupies must register as a real difference, because it is visible.
void darkShiftIsCaught() {
  const gfx::Image a = solid(32, 16, Color::rgb(0x1F1F1F));
  const gfx::Image b = solid(32, 16, Color::rgb(0x2B2B2B));
  const pixdiff::Result r = pixdiff::compare(a, b);
  CHECK_MSG(!r.passed, "0x1F1F1F vs 0x2B2B2B should fail: " + r.message);
  CHECK_MSG(r.maxDelta > 2.5,
            "expected a visible delta, got " + std::to_string(r.maxDelta));
}

// A single badly wrong pixel must fail even though it is a vanishing fraction
// of the image -- that is the missing-1px-separator case.
void oneSevereePixelFails() {
  gfx::Image a = solid(200, 40, Color::rgb(0x1F1F1F));
  const gfx::Image b = solid(200, 40, Color::rgb(0x1F1F1F));
  a.rowAt(20)[100] = gfx::premultiply(Color::rgb(0xFFFFFF));

  const pixdiff::Result r = pixdiff::compare(a, b);
  CHECK_MSG(!r.passed, "one white pixel in 8000 should fail: " + r.message);
  CHECK(r.worstPixel.x == 100 && r.worstPixel.y == 20);
}

// Sub-threshold noise across many pixels must still pass, or antialiasing
// differences between FreeType builds would make the suite useless.
void imperceptibleNoisePasses() {
  gfx::Image a = solid(64, 64, Color::rgb(0x808080));
  const gfx::Image b = solid(64, 64, Color::rgb(0x808080));
  for (int y = 0; y < 64; y += 2) {
    for (int x = 0; x < 64; x += 2) {
      a.rowAt(y)[x] = gfx::premultiply(Color::rgb(0x818181));
    }
  }
  const pixdiff::Result r = pixdiff::compare(a, b);
  CHECK_MSG(r.passed, "1/255 noise should pass: " + r.message);
}

void visualizationMarksTheDifference() {
  gfx::Image a = solid(16, 16, Color::rgb(0x000000));
  const gfx::Image b = solid(16, 16, Color::rgb(0x000000));
  a.rowAt(8)[8] = gfx::premultiply(Color::rgb(0xFFFFFF));

  const gfx::Image diff = pixdiff::visualize(a, b);
  CHECK(diff.valid());
  const uint32_t marked = diff.rowAt(8)[8];
  const uint32_t red = (marked >> 16) & 0xFF;
  const uint32_t green = (marked >> 8) & 0xFF;
  const uint32_t blue = marked & 0xFF;
  // Magenta: red and blue high, green absent.
  CHECK(red > 100 && blue > 100 && green == 0);
}

// Premultiplied storage must not make translucent colours compare as darker
// than they are.
void premultipliedAlphaRoundTrips() {
  gfx::Image a(8, 8);
  gfx::Image b(8, 8);
  a.fill(Color::rgba(0xFFFFFF, 0x80));
  b.fill(Color::rgba(0xFFFFFF, 0x80));
  const pixdiff::Result r = pixdiff::compare(a, b);
  CHECK_MSG(r.passed, r.message);
  CHECK(r.maxDelta < 1e-9);
}

}  // namespace

int main() {
  identicalImagesPass();
  sizeMismatchIsNotAPass();
  darkShiftIsCaught();
  oneSevereePixelFails();
  imperceptibleNoisePasses();
  visualizationMarksTheDifference();
  premultipliedAlphaRoundTrips();
  return finux::check::finish("pixdiff");
}

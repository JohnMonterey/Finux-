// Tests the acrylic pipeline: blur, tint, noise.
//
// Acrylic is easy to get subtly wrong in ways that still "look blurry", so
// these check the properties that matter rather than eyeballing output:
// that the blur conserves brightness instead of darkening towards edges, that
// it stays inside its region, that the result is opaque, and that the noise is
// reproducible (pixel-diff tests re-render and compare byte for byte).

#include <cmath>
#include <string>

#include "check.hpp"
#include "core/color.hpp"
#include "gfx/effects.hpp"
#include "gfx/image.hpp"

using namespace finux;
using namespace finux::gfx;

namespace {

Image solid(int w, int h, Color c) {
  Image image(w, h);
  image.fill(c);
  return image;
}

int channelR(uint32_t px) { return static_cast<int>((px >> 16) & 0xFF); }

double meanLuma(const Image& image, Rect region) {
  double total = 0.0;
  for (int y = region.y; y < region.bottom(); ++y) {
    const uint32_t* row = image.rowAt(y);
    for (int x = region.x; x < region.right(); ++x) {
      total += ((row[x] >> 16) & 0xFF) * 0.2126 +
               ((row[x] >> 8) & 0xFF) * 0.7152 + (row[x] & 0xFF) * 0.0722;
    }
  }
  return total / (region.w * region.h);
}

// A flat field must survive the blur unchanged. Edge handling that pads with
// zeros instead of clamping fails here, darkening the borders -- which on a
// taskbar reads as a shadow along every edge.
void blurPreservesFlatField() {
  Image image = solid(80, 60, Color::rgb(0x808080));
  effects::boxBlur(image, image.bounds(), 12);

  int minR = 255, maxR = 0;
  for (int y = 0; y < 60; ++y) {
    for (int x = 0; x < 80; ++x) {
      const int r = channelR(image.rowAt(y)[x]);
      minR = std::min(minR, r);
      maxR = std::max(maxR, r);
    }
  }
  CHECK_MSG(maxR - minR <= 1,
            "blur is not flat across a uniform field: " + std::to_string(minR) +
                ".." + std::to_string(maxR));
  CHECK_MSG(std::abs(minR - 0x80) <= 1,
            "blur shifted a uniform field's value to " + std::to_string(minR));
}

// The blur must not read or write outside its region.
void blurStaysInsideItsRegion() {
  Image image = solid(100, 100, Color::rgb(0x000000));
  // A bright block entirely inside the region to be blurred.
  for (int y = 40; y < 60; ++y) {
    for (int x = 40; x < 60; ++x) {
      image.rowAt(y)[x] = premultiply(Color::rgb(0xFFFFFF));
    }
  }

  const Rect region{30, 30, 40, 40};
  effects::boxBlur(image, region, 8);

  // Outside the region nothing may have changed.
  for (int y = 0; y < 100; ++y) {
    for (int x = 0; x < 100; ++x) {
      if (region.contains({x, y})) continue;
      if (image.rowAt(y)[x] != premultiply(Color::rgb(0x000000))) {
        CHECK_MSG(false, "blur wrote outside its region at " +
                             std::to_string(x) + "," + std::to_string(y));
        return;
      }
    }
  }
  // And inside it, the block must actually have spread.
  CHECK_MSG(channelR(image.rowAt(35)[35]) > 0,
            "blur did not spread brightness within its region");
}

// Blurring must not move the overall energy much: a bright square on a dark
// field keeps roughly the same mean.
void blurConservesMean() {
  Image image = solid(120, 120, Color::rgb(0x101010));
  for (int y = 40; y < 80; ++y) {
    for (int x = 40; x < 80; ++x) {
      image.rowAt(y)[x] = premultiply(Color::rgb(0xE0E0E0));
    }
  }
  const double before = meanLuma(image, image.bounds());
  effects::boxBlur(image, image.bounds(), 10);
  const double after = meanLuma(image, image.bounds());

  CHECK_MSG(std::abs(before - after) < 3.0,
            "blur changed the mean from " + std::to_string(before) + " to " +
                std::to_string(after));
}

// Acrylic output feeds straight to the X server, which has no alpha for the
// bar: a sub-255 alpha would punch a hole through the taskbar.
void acrylicOutputIsOpaque() {
  Image image(60, 40);
  image.fill(Color::rgba(0x3366AA, 0x80));

  effects::AcrylicParams params;
  params.blurRadius = 8;
  params.tint = Color::rgba(0xF3F3F3, 0xD4);
  effects::acrylic(image, image.bounds(), params);

  for (int y = 0; y < 40; ++y) {
    for (int x = 0; x < 60; ++x) {
      if ((image.rowAt(y)[x] >> 24) != 0xFF) {
        CHECK_MSG(false, "acrylic left a non-opaque pixel at " +
                             std::to_string(x) + "," + std::to_string(y));
        return;
      }
    }
  }
}

// The tint must actually pull the result towards the tint colour, and a
// higher alpha must pull it further. This is what distinguishes acrylic from
// "blur and hope".
void tintMovesTowardsTintColor() {
  const Color backdrop = Color::rgb(0x0A2A50);  // dark blue, like a wallpaper
  const Color tint = Color::rgb(0xF3F3F3);

  const auto luminanceWith = [&](uint8_t alpha) {
    Image image = solid(40, 40, backdrop);
    effects::AcrylicParams params;
    params.blurRadius = 4;
    params.noise = 0;
    params.tint = tint.withAlpha(alpha);
    effects::acrylic(image, image.bounds(), params);
    return meanLuma(image, image.bounds());
  };

  const double none = luminanceWith(0);
  const double partial = luminanceWith(0x80);
  const double heavy = luminanceWith(0xF0);

  CHECK_MSG(partial > none, "tint did not lighten towards the tint colour");
  CHECK_MSG(heavy > partial, "a higher tint alpha did not cover more");
  // At full-ish alpha the result should be close to the tint itself.
  CHECK_MSG(heavy > 200.0,
            "near-opaque tint did not approach the tint colour (got " +
                std::to_string(heavy) + ")");
}

// Noise must be reproducible: the pixel-diff harness renders the same frame
// twice and compares bytes, so a PRNG here would make every comparison fail.
void noiseIsDeterministic() {
  for (int i = 0; i < 200; ++i) {
    const int a = effects::noiseAt(i * 7, i * 13, 4);
    const int b = effects::noiseAt(i * 7, i * 13, 4);
    CHECK_MSG(a == b, "noiseAt is not deterministic");
    CHECK_MSG(a >= -4 && a <= 4,
              "noise out of range: " + std::to_string(a));
    if (finux::check::failures > 0) return;
  }
  CHECK_MSG(effects::noiseAt(3, 9, 0) == 0, "zero amplitude produced noise");

  // It must also vary; a constant would be deterministic but useless.
  bool varies = false;
  for (int i = 1; i < 50 && !varies; ++i) {
    if (effects::noiseAt(i, 0, 4) != effects::noiseAt(0, 0, 4)) varies = true;
  }
  CHECK_MSG(varies, "noise is constant across pixels");
}

// Two identical renders must be byte-identical.
void acrylicIsReproducible() {
  const auto render = [] {
    Image image = solid(50, 30, Color::rgb(0x224466));
    effects::AcrylicParams params;
    params.blurRadius = 6;
    effects::acrylic(image, image.bounds(), params);
    return image;
  };
  const Image a = render();
  const Image b = render();
  for (int y = 0; y < 30; ++y) {
    for (int x = 0; x < 50; ++x) {
      if (a.rowAt(y)[x] != b.rowAt(y)[x]) {
        CHECK_MSG(false, "acrylic is not reproducible at " + std::to_string(x) +
                             "," + std::to_string(y));
        return;
      }
    }
  }
}

// A radius wider than the region must not read wildly out of bounds or
// produce a degenerate result.
void hugeRadiusIsSafe() {
  Image image = solid(20, 6, Color::rgb(0x4080C0));
  effects::boxBlur(image, image.bounds(), 500);
  CHECK_MSG(std::abs(channelR(image.rowAt(3)[10]) - 0x40) <= 2,
            "an oversized radius distorted a flat field");
}

}  // namespace

int main() {
  blurPreservesFlatField();
  blurStaysInsideItsRegion();
  blurConservesMean();
  acrylicOutputIsOpaque();
  tintMovesTowardsTintColor();
  noiseIsDeterministic();
  acrylicIsReproducible();
  hugeRadiusIsSafe();
  return finux::check::finish("effects");
}

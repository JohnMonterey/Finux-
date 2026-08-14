#pragma once

#include <string>

#include "gfx/image.hpp"

namespace finux::pixdiff {

struct Options {
  // Per-pixel CIE76 tolerance. A ΔE of ~1.0 is the classic "just noticeable
  // difference" threshold; anything under 2.0 will not be seen side by side.
  // Antialiased glyph edges routinely land near 2, so the default is chosen to
  // accept resampling noise while still failing on a 1px geometry shift --
  // which moves whole stems and produces ΔE well into the tens.
  double perPixelTolerance = 2.5;

  // Fraction of pixels permitted to exceed the tolerance. Not zero, because a
  // handful of glyph edge pixels differ between FreeType point releases.
  double maxFailingFraction = 0.002;

  // Hard ceiling on any single pixel, so a small but severe defect (a missing
  // 1px separator, say) cannot hide under maxFailingFraction.
  double maxSingleDelta = 24.0;
};

struct Result {
  bool sizesMatch = false;
  bool passed = false;
  int comparedPixels = 0;
  int failingPixels = 0;
  double failingFraction = 0.0;
  double meanDelta = 0.0;
  double maxDelta = 0.0;
  Point worstPixel;
  std::string message;
};

// Compares `actual` against `expected` in CIE L*a*b*.
//
// Working in Lab rather than raw RGB matters for shell chrome specifically:
// the taskbar is near-black, and equal RGB steps down there are far more
// visible than the same steps in midtones. An RGB metric would happily pass a
// visibly wrong dark grey while failing an imperceptible highlight shift.
Result compare(const gfx::Image& actual, const gfx::Image& expected,
               const Options& options = {});

// Renders a magenta-over-grayscale visualisation of where the two differ.
// Returns an invalid image if the sizes do not match.
gfx::Image visualize(const gfx::Image& actual, const gfx::Image& expected,
                     const Options& options = {});

// Compares against a stored reference PNG.
//
// When the reference is absent the actual output is written next to it as
// `<name>.actual.png` and the result carries `passed == false` with an
// explanatory message; callers decide whether a missing reference is a skip
// (it is, for a fresh checkout) or a failure (it is, in CI).
struct FileResult {
  Result result;
  bool referenceExisted = false;
  std::string actualPath;
  std::string diffPath;
};

FileResult compareToReference(const gfx::Image& actual,
                              const std::string& referenceDir,
                              const std::string& name,
                              const Options& options = {});

}  // namespace finux::pixdiff

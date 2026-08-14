#pragma once

#include "core/color.hpp"
#include "core/geometry.hpp"
#include "gfx/image.hpp"

namespace finux::gfx::effects {

// Windows acrylic, as the taskbar and Start menu use it.
//
// Acrylic is not "draw the surface at 80% alpha". It is a composition: blur
// what is behind the surface, tint the result, and add a little noise so large
// flat areas do not band. Skipping the blur and simply lowering alpha is the
// single most common way a Windows-alike gets this wrong -- wallpaper detail
// shows straight through and text sitting on top becomes unreadable over busy
// backgrounds, which is exactly what the blur exists to prevent.
struct AcrylicParams {
  // Blur radius in pixels. Windows uses a large radius; anything under ~15
  // still reads as "a translucent panel" rather than as frosted glass.
  int blurRadius = 30;
  // Tint colour, with alpha. The alpha is how much of the tint covers the
  // blurred backdrop.
  Color tint = Color::rgba(0xF3F3F3, 0xD9);
  // Peak amplitude of the noise, in 8-bit levels. Windows' is subtle.
  int noise = 4;
};

// Separable box blur, applied three times to approximate a Gaussian.
//
// Operates on premultiplied pixels, which is what makes it correct: averaging
// straight-alpha colour would let fully transparent pixels drag their
// (meaningless) colour into their neighbours.
void boxBlur(Image& image, Rect region, int radius);

// Blur plus noise, no tint: the "behind" half of acrylic.
//
// Split from the tint because a surface may carry several tints over one
// blurred backdrop -- the Start menu's rail, app list and tile panel are three
// different tints over a single blur, and blurring each separately would show
// a seam where their edges meet.
void blurBackdrop(Image& image, Rect region, int radius, int noise);

// Blur, tint and noise in one call, for a surface with a single uniform tint.
void acrylic(Image& image, Rect region, const AcrylicParams& params);

// Deterministic value noise in [-amplitude, amplitude] for a pixel.
//
// Deliberately a hash of the coordinates rather than a PRNG: the pixel-diff
// tests re-render the same frame and compare byte for byte, so noise has to be
// reproducible across runs and across processes.
int noiseAt(int x, int y, int amplitude);

}  // namespace finux::gfx::effects

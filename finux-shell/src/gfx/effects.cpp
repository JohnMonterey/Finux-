#include "gfx/effects.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace finux::gfx::effects {
namespace {

// One separable box-blur pass over a single axis.
//
// A sliding window sum keeps this O(pixels) rather than O(pixels * radius),
// which matters: the Start menu backdrop is ~600k pixels and gets three passes
// per axis on every repaint.
void blurAxis(std::vector<uint32_t>& src, std::vector<uint32_t>& dst, int width,
              int height, int radius, bool horizontal) {
  const int lineLength = horizontal ? width : height;
  const int lineCount = horizontal ? height : width;
  const int window = radius * 2 + 1;

  for (int line = 0; line < lineCount; ++line) {
    const auto at = [&](int i) -> uint32_t& {
      return horizontal ? src[static_cast<size_t>(line) * width + i]
                        : src[static_cast<size_t>(i) * width + line];
    };
    const auto out = [&](int i) -> uint32_t& {
      return horizontal ? dst[static_cast<size_t>(line) * width + i]
                        : dst[static_cast<size_t>(i) * width + line];
    };

    // Seed the window with the clamped left edge, so the blur does not darken
    // towards the borders the way a zero-padded one would.
    int sumA = 0, sumR = 0, sumG = 0, sumB = 0;
    for (int i = -radius; i <= radius; ++i) {
      const uint32_t px = at(std::clamp(i, 0, lineLength - 1));
      sumA += (px >> 24) & 0xFF;
      sumR += (px >> 16) & 0xFF;
      sumG += (px >> 8) & 0xFF;
      sumB += px & 0xFF;
    }

    for (int i = 0; i < lineLength; ++i) {
      out(i) = (static_cast<uint32_t>(sumA / window) << 24) |
               (static_cast<uint32_t>(sumR / window) << 16) |
               (static_cast<uint32_t>(sumG / window) << 8) |
               static_cast<uint32_t>(sumB / window);

      const uint32_t leaving = at(std::clamp(i - radius, 0, lineLength - 1));
      const uint32_t entering =
          at(std::clamp(i + radius + 1, 0, lineLength - 1));
      sumA += static_cast<int>((entering >> 24) & 0xFF) -
              static_cast<int>((leaving >> 24) & 0xFF);
      sumR += static_cast<int>((entering >> 16) & 0xFF) -
              static_cast<int>((leaving >> 16) & 0xFF);
      sumG += static_cast<int>((entering >> 8) & 0xFF) -
              static_cast<int>((leaving >> 8) & 0xFF);
      sumB += static_cast<int>(entering & 0xFF) -
              static_cast<int>(leaving & 0xFF);
    }
  }
}

}  // namespace

int noiseAt(int x, int y, int amplitude) {
  if (amplitude <= 0) return 0;
  // A cheap integer hash with good enough avalanche for visual dither.
  uint32_t h = static_cast<uint32_t>(x) * 0x9E3779B1u ^
               static_cast<uint32_t>(y) * 0x85EBCA77u;
  h ^= h >> 15;
  h *= 0xC2B2AE3Du;
  h ^= h >> 13;
  const int span = amplitude * 2 + 1;
  return static_cast<int>(h % static_cast<uint32_t>(span)) - amplitude;
}

void boxBlur(Image& image, Rect region, int radius) {
  if (!image.valid() || radius <= 0) return;
  const Rect box = region.intersected(image.bounds());
  if (box.empty()) return;

  // Clamp so the window cannot exceed the region; a radius wider than the
  // region would otherwise average almost entirely edge-clamped samples.
  const int r = std::min(radius, std::max(1, std::min(box.w, box.h) / 2));

  const int stride = image.strideWords();
  std::vector<uint32_t> a(static_cast<size_t>(box.w) * box.h);
  std::vector<uint32_t> b(a.size());

  for (int y = 0; y < box.h; ++y) {
    const uint32_t* row = image.rowAt(box.y + y) + box.x;
    std::copy(row, row + box.w, a.begin() + static_cast<size_t>(y) * box.w);
  }

  // Three box passes approximate a Gaussian closely enough that the difference
  // is invisible at these radii, at a fraction of the cost.
  for (int pass = 0; pass < 3; ++pass) {
    blurAxis(a, b, box.w, box.h, r, true);
    blurAxis(b, a, box.w, box.h, r, false);
  }

  for (int y = 0; y < box.h; ++y) {
    uint32_t* row = image.rowAt(box.y + y) + box.x;
    const uint32_t* src = a.data() + static_cast<size_t>(y) * box.w;
    std::copy(src, src + box.w, row);
  }
  (void)stride;
}

void blurBackdrop(Image& image, Rect region, int radius, int noise) {
  if (!image.valid()) return;
  const Rect box = region.intersected(image.bounds());
  if (box.empty()) return;

  boxBlur(image, box, radius);
  if (noise <= 0) return;

  for (int y = box.y; y < box.bottom(); ++y) {
    uint32_t* row = image.rowAt(y);
    for (int x = box.x; x < box.right(); ++x) {
      const uint32_t px = row[x];
      const int n = noiseAt(x, y, noise);
      const int r = std::clamp(static_cast<int>((px >> 16) & 0xFF) + n, 0, 255);
      const int g = std::clamp(static_cast<int>((px >> 8) & 0xFF) + n, 0, 255);
      const int b = std::clamp(static_cast<int>(px & 0xFF) + n, 0, 255);
      row[x] = (px & 0xFF000000u) | (static_cast<uint32_t>(r) << 16) |
               (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
    }
  }
}

void acrylic(Image& image, Rect region, const AcrylicParams& params) {
  if (!image.valid()) return;
  const Rect box = region.intersected(image.bounds());
  if (box.empty()) return;

  boxBlur(image, box, params.blurRadius);

  const int tintAlpha = params.tint.a;
  for (int y = box.y; y < box.bottom(); ++y) {
    uint32_t* row = image.rowAt(y);
    for (int x = box.x; x < box.right(); ++x) {
      const uint32_t dst = row[x];
      int dr = static_cast<int>((dst >> 16) & 0xFF);
      int dg = static_cast<int>((dst >> 8) & 0xFF);
      int db = static_cast<int>(dst & 0xFF);

      // Source-over of the tint, both sides premultiplied.
      const auto over = [tintAlpha](int srcColor, int dstPremul) {
        return (srcColor * tintAlpha + dstPremul * (255 - tintAlpha) + 127) /
               255;
      };
      dr = over(params.tint.r, dr);
      dg = over(params.tint.g, dg);
      db = over(params.tint.b, db);

      if (params.noise > 0) {
        const int n = noiseAt(x, y, params.noise);
        dr += n;
        dg += n;
        db += n;
      }

      // The shell's surfaces are opaque once composed: the blurred backdrop is
      // already part of the image, so leaving alpha below 255 here would punch
      // a hole through the taskbar when X presents it.
      row[x] = 0xFF000000u |
               (static_cast<uint32_t>(std::clamp(dr, 0, 255)) << 16) |
               (static_cast<uint32_t>(std::clamp(dg, 0, 255)) << 8) |
               static_cast<uint32_t>(std::clamp(db, 0, 255));
    }
  }
}

}  // namespace finux::gfx::effects

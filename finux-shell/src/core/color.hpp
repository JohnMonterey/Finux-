#pragma once

#include <cstdint>

namespace finux {

// Non-premultiplied 8-bit sRGB + alpha. Conversion to the premultiplied form
// the framebuffer wants happens at blit time, in one place, so that alpha
// compositing bugs cannot spread through the widget code.
struct Color {
  uint8_t r = 0, g = 0, b = 0, a = 255;

  static constexpr Color rgb(uint32_t hex) {
    return {static_cast<uint8_t>((hex >> 16) & 0xFF),
            static_cast<uint8_t>((hex >> 8) & 0xFF),
            static_cast<uint8_t>(hex & 0xFF), 255};
  }
  static constexpr Color rgba(uint32_t hex, uint8_t alpha) {
    Color c = rgb(hex);
    c.a = alpha;
    return c;
  }
  // 0..100 opacity, matching how the Windows shell colours are usually specced.
  static constexpr Color rgbPercent(uint32_t hex, int percent) {
    return rgba(hex, static_cast<uint8_t>((percent * 255 + 50) / 100));
  }

  constexpr Color withAlpha(uint8_t alpha) const { return {r, g, b, alpha}; }

  // 0xAARRGGBB, the layout Blend2D's BLRgba32 expects.
  constexpr uint32_t argb() const {
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | b;
  }

  constexpr bool transparent() const { return a == 0; }

  friend constexpr bool operator==(Color, Color) = default;
};

// Source-over of a straight-alpha colour onto an opaque backdrop. Used to
// resolve the shell's translucent hover layers into flat reference colours.
constexpr Color flatten(Color src, Color backdrop) {
  const int sa = src.a;
  const auto mix = [sa](uint8_t s, uint8_t d) {
    return static_cast<uint8_t>((s * sa + d * (255 - sa) + 127) / 255);
  };
  return {mix(src.r, backdrop.r), mix(src.g, backdrop.g), mix(src.b, backdrop.b),
          255};
}

}  // namespace finux

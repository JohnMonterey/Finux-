#pragma once

#include <blend2d/blend2d.h>

#include <cstdint>
#include <string>

#include "core/color.hpp"
#include "core/geometry.hpp"

namespace finux::gfx {

// A 32-bit premultiplied BGRA surface (BL_FORMAT_PRGB32), which is both what
// Blend2D rasterises into and what X11 wants for a 24/32-bit TrueColor visual
// on a little-endian host. Keeping one format everywhere means the taskbar can
// be blitted to the screen without a conversion pass.
class Image {
 public:
  Image() = default;
  Image(int width, int height);

  bool valid() const { return !impl_.is_empty(); }
  int width() const { return impl_.width(); }
  int height() const { return impl_.height(); }
  Size size() const { return {width(), height()}; }
  Rect bounds() const { return {0, 0, width(), height()}; }

  BLImage& bl() { return impl_; }
  const BLImage& bl() const { return impl_; }

  // Raw access, for the glyph blitter which composites per channel and so
  // cannot go through Blend2D's paint pipeline.
  uint32_t* pixels();
  const uint32_t* pixels() const;
  // Distance between rows in uint32_t units, not bytes.
  int strideWords() const;

  uint32_t* rowAt(int y) { return pixels() + static_cast<ptrdiff_t>(y) * strideWords(); }
  const uint32_t* rowAt(int y) const {
    return pixels() + static_cast<ptrdiff_t>(y) * strideWords();
  }

  void fill(Color c);

  bool savePng(const std::string& path) const;
  static Image loadPng(const std::string& path);

 private:
  BLImage impl_;
};

// Premultiply a straight-alpha colour into the 0xAARRGGBB word the surface
// stores. Every write into an Image goes through here.
constexpr uint32_t premultiply(Color c) {
  const uint32_t a = c.a;
  const auto ch = [a](uint8_t v) -> uint32_t { return (v * a + 127) / 255; };
  return (a << 24) | (ch(c.r) << 16) | (ch(c.g) << 8) | ch(c.b);
}

}  // namespace finux::gfx

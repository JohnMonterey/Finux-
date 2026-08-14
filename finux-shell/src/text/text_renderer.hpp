#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/color.hpp"
#include "core/geometry.hpp"
#include "gfx/image.hpp"
#include "text/font.hpp"

namespace finux::text {

enum class HAlign { Left, Center, Right };
enum class VAlign { Top, Middle, Baseline, Bottom };

struct ShapedGlyph {
  uint32_t index = 0;
  int xOffset = 0;
  int yOffset = 0;
  int xAdvance = 0;
};

struct ShapedText {
  std::vector<ShapedGlyph> glyphs;
  int width = 0;
};

// Shapes with HarfBuzz, rasterises with FreeType, and composites glyph
// coverage directly into the target surface.
//
// Text bypasses Blend2D on purpose: subpixel (ClearType-style) output needs
// independent per-channel coverage, which a paint pipeline built around a
// single alpha mask cannot express. Owning the blit is what makes the
// Windows look reachable at all.
class TextRenderer {
 public:
  explicit TextRenderer(FontLibrary& library);

  ShapedText shape(const Font& font, std::string_view utf8);
  int measure(const Font& font, std::string_view utf8);

  // `origin` is the text baseline's left end, which is the coordinate space
  // FreeType and HarfBuzz both work in.
  void drawShaped(gfx::Image& target, Point origin, const Font& font,
                  const ShapedText& text, Color color, Rect clip);

  // Lays out a single line inside `box`. When `ellipsize` is set and the text
  // does not fit, it is trimmed and a U+2026 appended -- the same treatment
  // taskbar buttons and Explorer columns give overlong labels.
  void drawInBox(gfx::Image& target, Rect box, const Font& font,
                 std::string_view utf8, Color color, HAlign h = HAlign::Left,
                 VAlign v = VAlign::Middle, bool ellipsize = true);

  // Trims `utf8` so it fits `maxWidth`, appending an ellipsis if trimmed.
  std::string ellipsize(const Font& font, std::string_view utf8, int maxWidth);

 private:
  struct GlyphKey {
    const void* face;
    uint32_t index;
    int mode;
    bool operator==(const GlyphKey&) const = default;
  };
  struct GlyphKeyHash {
    size_t operator()(const GlyphKey& k) const noexcept {
      return std::hash<const void*>{}(k.face) ^ (std::hash<uint32_t>{}(k.index) << 1) ^
             (std::hash<int>{}(k.mode) << 3);
    }
  };
  // A rasterised glyph. `coverage` holds one byte per channel per pixel for
  // subpixel modes (channels == 3) and one byte per pixel otherwise.
  struct Glyph {
    std::vector<uint8_t> coverage;
    int width = 0;   // in output pixels, not coverage bytes
    int height = 0;
    int channels = 1;
    int bearingX = 0;
    int bearingY = 0;
  };

  const Glyph* glyphFor(const Font& font, uint32_t index);
  void buildGammaTable(double gamma);

  FontLibrary& library_;
  std::unordered_map<GlyphKey, Glyph, GlyphKeyHash> glyphs_;
  double gammaValue_ = 0.0;
  uint8_t gamma_[256] = {};
};

}  // namespace finux::text

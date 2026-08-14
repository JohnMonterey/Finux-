#include "text/text_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace finux::text {
namespace {

constexpr int kF26Dot6 = 64;
constexpr const char* kEllipsis = "\xE2\x80\xA6";  // U+2026

int loadFlagsFor(const FontOptions& o) {
  int flags = FT_LOAD_DEFAULT;
  if (o.lightHinting) {
    flags |= FT_LOAD_TARGET_LIGHT;
  } else if (o.antialias == Antialias::Grayscale) {
    flags |= FT_LOAD_TARGET_NORMAL;
  } else {
    flags |= FT_LOAD_TARGET_LCD;
  }
  return flags;
}

FT_Render_Mode renderModeFor(const FontOptions& o) {
  switch (o.antialias) {
    case Antialias::Grayscale:
      return FT_RENDER_MODE_NORMAL;
    case Antialias::SubpixelRgb:
    case Antialias::SubpixelBgr:
      return FT_RENDER_MODE_LCD;
  }
  return FT_RENDER_MODE_NORMAL;
}

// Number of bytes to step back from the end of a UTF-8 string to drop one
// whole code point.
size_t trimOneCodepoint(std::string_view s) {
  if (s.empty()) return 0;
  size_t i = s.size() - 1;
  while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
  return i;
}

}  // namespace

TextRenderer::TextRenderer(FontLibrary& library) : library_(library) {}

void TextRenderer::buildGammaTable(double gamma) {
  if (gamma == gammaValue_) return;
  gammaValue_ = gamma;
  const double inv = (gamma > 0.0) ? 1.0 / gamma : 1.0;
  for (int i = 0; i < 256; ++i) {
    const double linear = i / 255.0;
    gamma_[i] = static_cast<uint8_t>(
        std::lround(std::pow(linear, inv) * 255.0));
  }
}

ShapedText TextRenderer::shape(const Font& font, std::string_view utf8) {
  ShapedText out;
  if (utf8.empty() || !font.hbFont()) return out;

  hb_buffer_t* buffer = hb_buffer_create();
  hb_buffer_add_utf8(buffer, utf8.data(), static_cast<int>(utf8.size()), 0,
                     static_cast<int>(utf8.size()));
  // Explicit properties: guessing costs a segment-property scan per call and
  // silently changes shaping when the string is punctuation-only.
  hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
  hb_buffer_set_script(buffer, HB_SCRIPT_LATIN);
  hb_buffer_set_language(buffer, hb_language_from_string("en", -1));

  hb_shape(font.hbFont(), buffer, nullptr, 0);

  unsigned count = 0;
  const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buffer, &count);
  const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buffer, &count);

  out.glyphs.reserve(count);
  int pen = 0;
  for (unsigned i = 0; i < count; ++i) {
    ShapedGlyph g;
    g.index = info[i].codepoint;
    // hb-ft reports 26.6 fixed point because the FT_Face carries a pixel size.
    g.xOffset = pos[i].x_offset / kF26Dot6;
    g.yOffset = pos[i].y_offset / kF26Dot6;
    g.xAdvance = pos[i].x_advance / kF26Dot6;
    pen += g.xAdvance;
    out.glyphs.push_back(g);
  }
  out.width = pen;

  hb_buffer_destroy(buffer);
  return out;
}

int TextRenderer::measure(const Font& font, std::string_view utf8) {
  return shape(font, utf8).width;
}

const TextRenderer::Glyph* TextRenderer::glyphFor(const Font& font,
                                                  uint32_t index) {
  const GlyphKey key{font.ftFace(), index,
                     static_cast<int>(font.options().antialias)};
  if (auto it = glyphs_.find(key); it != glyphs_.end()) return &it->second;

  FT_Face face = font.ftFace();
  if (!face) return nullptr;
  if (FT_Load_Glyph(face, index, loadFlagsFor(font.options())) != 0) return nullptr;
  if (FT_Render_Glyph(face->glyph, renderModeFor(font.options())) != 0) {
    return nullptr;
  }

  const FT_Bitmap& bmp = face->glyph->bitmap;
  Glyph glyph;
  glyph.channels = (bmp.pixel_mode == FT_PIXEL_MODE_LCD) ? 3 : 1;
  glyph.width = static_cast<int>(bmp.width) / glyph.channels;
  glyph.height = static_cast<int>(bmp.rows);
  glyph.bearingX = face->glyph->bitmap_left;
  glyph.bearingY = face->glyph->bitmap_top;

  // FT_Bitmap rows are `pitch` bytes apart and pitch may be negative for
  // bottom-up bitmaps; copy into a tightly packed top-down buffer so the
  // blitter never has to think about it again.
  const size_t rowBytes = static_cast<size_t>(glyph.width) * glyph.channels;
  glyph.coverage.assign(rowBytes * glyph.height, 0);
  for (int y = 0; y < glyph.height; ++y) {
    const unsigned char* src = bmp.buffer + static_cast<ptrdiff_t>(y) * bmp.pitch;
    std::memcpy(glyph.coverage.data() + rowBytes * y, src, rowBytes);
  }

  return &glyphs_.emplace(key, std::move(glyph)).first->second;
}

void TextRenderer::drawShaped(gfx::Image& target, Point origin, const Font& font,
                              const ShapedText& text, Color color, Rect clip) {
  if (!target.valid() || color.transparent()) return;
  buildGammaTable(font.options().gamma);

  const Rect bounds = clip.intersected(target.bounds());
  if (bounds.empty()) return;

  const bool bgr = font.options().antialias == Antialias::SubpixelBgr;
  const int stride = target.strideWords();
  uint32_t* base = target.pixels();
  if (!base) return;

  int pen = origin.x;
  for (const ShapedGlyph& sg : text.glyphs) {
    const Glyph* glyph = glyphFor(font, sg.index);
    if (!glyph) {
      pen += sg.xAdvance;
      continue;
    }

    const int gx = pen + sg.xOffset + glyph->bearingX;
    const int gy = origin.y - sg.yOffset - glyph->bearingY;

    // Clip the glyph box to the target before touching any pixels.
    const int x0 = std::max(bounds.left(), gx);
    const int y0 = std::max(bounds.top(), gy);
    const int x1 = std::min(bounds.right(), gx + glyph->width);
    const int y1 = std::min(bounds.bottom(), gy + glyph->height);

    const size_t rowBytes = static_cast<size_t>(glyph->width) * glyph->channels;
    for (int y = y0; y < y1; ++y) {
      const uint8_t* covRow =
          glyph->coverage.data() + rowBytes * static_cast<size_t>(y - gy);
      uint32_t* dstRow = base + static_cast<ptrdiff_t>(y) * stride;

      for (int x = x0; x < x1; ++x) {
        const int sx = x - gx;

        int cr, cg, cb;
        if (glyph->channels == 3) {
          const uint8_t* c = covRow + static_cast<size_t>(sx) * 3;
          cr = gamma_[bgr ? c[2] : c[0]];
          cg = gamma_[c[1]];
          cb = gamma_[bgr ? c[0] : c[2]];
        } else {
          cr = cg = cb = gamma_[covRow[sx]];
        }

        // Fold the requested text alpha into the per-channel coverage.
        if (color.a != 255) {
          cr = cr * color.a / 255;
          cg = cg * color.a / 255;
          cb = cb * color.a / 255;
        }
        if ((cr | cg | cb) == 0) continue;

        const uint32_t dst = dstRow[x];
        const int da = static_cast<int>((dst >> 24) & 0xFF);
        const int dr = static_cast<int>((dst >> 16) & 0xFF);
        const int dg = static_cast<int>((dst >> 8) & 0xFF);
        const int db = static_cast<int>(dst & 0xFF);

        // Source-over with premultiplied destination. The source contribution
        // is colour*coverage, which is already premultiplied by construction.
        const auto blend = [](int srcColor, int cov, int dstPremul) {
          return (srcColor * cov + dstPremul * (255 - cov) + 127) / 255;
        };
        const int nr = blend(color.r, cr, dr);
        const int ng = blend(color.g, cg, dg);
        const int nb = blend(color.b, cb, db);

        // One alpha for three coverages: average them. Exact per-channel alpha
        // would need a destination that stores it, which PRGB32 does not.
        const int ca = (cr + cg + cb) / 3;
        const int na = ca + (da * (255 - ca) + 127) / 255;

        dstRow[x] = (static_cast<uint32_t>(std::min(na, 255)) << 24) |
                    (static_cast<uint32_t>(std::min(nr, 255)) << 16) |
                    (static_cast<uint32_t>(std::min(ng, 255)) << 8) |
                    static_cast<uint32_t>(std::min(nb, 255));
      }
    }
    pen += sg.xAdvance;
  }
}

std::string TextRenderer::ellipsize(const Font& font, std::string_view utf8,
                                    int maxWidth) {
  if (maxWidth <= 0) return {};
  if (measure(font, utf8) <= maxWidth) return std::string(utf8);

  const int ellipsisWidth = measure(font, kEllipsis);
  if (ellipsisWidth > maxWidth) return {};

  std::string_view head = utf8;
  while (!head.empty()) {
    head = head.substr(0, trimOneCodepoint(head));
    if (measure(font, head) + ellipsisWidth <= maxWidth) break;
  }
  return std::string(head) + kEllipsis;
}

void TextRenderer::drawInBox(gfx::Image& target, Rect box, const Font& font,
                             std::string_view utf8, Color color, HAlign h,
                             VAlign v, bool doEllipsize) {
  if (box.empty() || utf8.empty()) return;

  const std::string text =
      doEllipsize ? ellipsize(font, utf8, box.w) : std::string(utf8);
  if (text.empty()) return;

  const ShapedText shaped = shape(font, text);

  int x = box.x;
  switch (h) {
    case HAlign::Left: break;
    case HAlign::Center: x = box.x + (box.w - shaped.width) / 2; break;
    case HAlign::Right: x = box.right() - shaped.width; break;
  }

  int baseline = box.y + font.ascent();
  switch (v) {
    case VAlign::Top: break;
    case VAlign::Baseline: baseline = box.y; break;
    case VAlign::Bottom: baseline = box.bottom() - font.descent(); break;
    case VAlign::Middle:
      // Centre the ascent+descent box, not the line box: Win10 shell labels
      // sit optically centred, and using lineHeight here drops them ~1px low.
      baseline = box.y + (box.h - (font.ascent() + font.descent())) / 2 +
                 font.ascent();
      break;
  }

  drawShaped(target, {x, baseline}, font, shaped, color, box);
}

}  // namespace finux::text

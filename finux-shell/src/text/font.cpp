#include "text/font.hpp"

#include FT_LCD_FILTER_H

#include <fontconfig/fontconfig.h>
#include <hb-ft.h>

#include <cstring>

namespace finux::text {
namespace {

// FreeType reports scaled metrics in 26.6 fixed point.
constexpr int kF26Dot6 = 64;
int ceil26_6(FT_Pos v) { return static_cast<int>((v + kF26Dot6 - 1) / kF26Dot6); }

}  // namespace

Font::~Font() {
  if (hbFont_) hb_font_destroy(hbFont_);
  if (face_) FT_Done_Face(face_);
}

FontLibrary::FontLibrary() {
  if (FT_Init_FreeType(&ft_) != 0) {
    ft_ = nullptr;
    return;
  }
  // Without an LCD filter, subpixel rendering produces heavy colour fringing.
  // FreeType builds without subpixel support return an error here; we fall
  // back to grayscale at render time rather than shipping fringed text.
  lcdFilterAvailable_ = FT_Library_SetLcdFilter(ft_, FT_LCD_FILTER_DEFAULT) == 0;
  FcInit();
}

FontLibrary::~FontLibrary() {
  cache_.clear();
  if (ft_) FT_Done_FreeType(ft_);
}

const std::vector<std::string>& FontLibrary::uiFontChain() {
  static const std::vector<std::string> chain = {"Segoe UI", "Selawik",
                                                 "DejaVu Sans"};
  return chain;
}

std::optional<std::string> FontLibrary::resolveFamilyFile(
    const std::string& family) const {
  FcPattern* pattern = FcNameParse(
      reinterpret_cast<const FcChar8*>(family.c_str()));
  if (!pattern) return std::nullopt;

  FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
  FcDefaultSubstitute(pattern);

  FcResult result = FcResultNoMatch;
  FcPattern* matched = FcFontMatch(nullptr, pattern, &result);
  FcPatternDestroy(pattern);
  if (!matched || result != FcResultMatch) {
    if (matched) FcPatternDestroy(matched);
    return std::nullopt;
  }

  // fontconfig always returns *something*; a request for "Segoe UI" on a
  // machine without it happily yields DejaVu. Compare the family it actually
  // matched so the caller's fallback chain stays meaningful.
  FcChar8* matchedFamily = nullptr;
  const bool haveFamily =
      FcPatternGetString(matched, FC_FAMILY, 0, &matchedFamily) == FcResultMatch;
  if (!haveFamily ||
      strcasecmp(reinterpret_cast<const char*>(matchedFamily),
                 family.c_str()) != 0) {
    FcPatternDestroy(matched);
    return std::nullopt;
  }

  FcChar8* file = nullptr;
  std::optional<std::string> out;
  if (FcPatternGetString(matched, FC_FILE, 0, &file) == FcResultMatch && file) {
    out = std::string(reinterpret_cast<const char*>(file));
  }
  FcPatternDestroy(matched);
  return out;
}

std::shared_ptr<Font> FontLibrary::open(const std::vector<std::string>& families,
                                        int pixelSize, FontOptions options) {
  if (!valid() || pixelSize <= 0) return nullptr;

  if (options.antialias != Antialias::Grayscale && !lcdFilterAvailable_) {
    options.antialias = Antialias::Grayscale;
  }

  for (const std::string& family : families) {
    std::optional<std::string> file = resolveFamilyFile(family);
    if (!file) continue;

    const std::string key = *file + "|" + std::to_string(pixelSize) + "|" +
                            std::to_string(static_cast<int>(options.antialias)) +
                            "|" + std::to_string(options.lightHinting);
    if (auto it = cache_.find(key); it != cache_.end()) {
      if (auto existing = it->second.lock()) return existing;
    }

    FT_Face face = nullptr;
    if (FT_New_Face(ft_, file->c_str(), 0, &face) != 0) continue;
    if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixelSize)) != 0) {
      FT_Done_Face(face);
      continue;
    }

    std::shared_ptr<Font> font(new Font());
    font->face_ = face;
    font->family_ = family;
    font->file_ = *file;
    font->pixelSize_ = pixelSize;
    font->options_ = options;
    font->ascent_ = ceil26_6(face->size->metrics.ascender);
    font->descent_ = ceil26_6(-face->size->metrics.descender);
    font->lineHeight_ = ceil26_6(face->size->metrics.height);

    font->hbFont_ = hb_ft_font_create_referenced(face);
    if (!font->hbFont_) continue;  // ~Font releases the face
    hb_ft_font_set_funcs(font->hbFont_);

    cache_[key] = font;
    return font;
  }
  return nullptr;
}

}  // namespace finux::text

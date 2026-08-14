#pragma once

#include <ft2build.h>
#include FT_FREETYPE_H

#include <hb.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace finux::text {

// How glyph coverage is turned into pixels.
//
// Windows 10's shell draws UI text with ClearType, i.e. horizontal RGB
// subpixel coverage plus a gamma curve that makes stems noticeably darker
// than a naive linear blend. Matching that is the single largest contributor
// to "this looks like Windows" -- far more than any individual widget metric,
// because text is on screen everywhere at once.
enum class Antialias {
  Grayscale,  // 8-bit coverage; correct for rotated/scaled text and for tests
  SubpixelRgb,  // ClearType-alike, horizontal RGB stripe order
  SubpixelBgr,
};

struct FontOptions {
  Antialias antialias = Antialias::SubpixelRgb;

  // Vertical-only hinting. Windows' "natural" rendering does not snap stems
  // horizontally, which is why full FreeType hinting immediately reads as
  // "Linux" even with the right typeface.
  bool lightHinting = true;

  // Text gamma. Windows' default ClearType contrast lands around 1.8; pure
  // linear coverage (1.0) renders visibly thinner than the reference.
  double gamma = 1.8;
};

// A resolved, sized face. Not thread-safe: FT_Face carries a mutable glyph
// slot, so one Font belongs to one thread.
class Font {
 public:
  ~Font();
  Font(const Font&) = delete;
  Font& operator=(const Font&) = delete;

  int pixelSize() const { return pixelSize_; }
  const std::string& family() const { return family_; }
  const std::string& file() const { return file_; }
  const FontOptions& options() const { return options_; }

  // Rounded up from FreeType's 26.6 metrics, matching how the shell lays out
  // single-line labels on whole pixels.
  int ascent() const { return ascent_; }
  int descent() const { return descent_; }
  int lineHeight() const { return lineHeight_; }

  FT_Face ftFace() const { return face_; }
  hb_font_t* hbFont() const { return hbFont_; }

 private:
  friend class FontLibrary;
  Font() = default;

  FT_Face face_ = nullptr;
  hb_font_t* hbFont_ = nullptr;
  std::string family_;
  std::string file_;
  int pixelSize_ = 0;
  int ascent_ = 0;
  int descent_ = 0;
  int lineHeight_ = 0;
  FontOptions options_;
};

// Owns FT_Library and resolves family names to files through fontconfig.
class FontLibrary {
 public:
  FontLibrary();
  ~FontLibrary();
  FontLibrary(const FontLibrary&) = delete;
  FontLibrary& operator=(const FontLibrary&) = delete;

  bool valid() const { return ft_ != nullptr; }

  // Resolves the first family in `families` that fontconfig can satisfy with a
  // real match, falling back to the next. Returns nullptr only if every
  // candidate fails.
  std::shared_ptr<Font> open(const std::vector<std::string>& families,
                             int pixelSize, FontOptions options = {});
  std::shared_ptr<Font> open(const std::string& family, int pixelSize,
                             FontOptions options = {}) {
    return open(std::vector<std::string>{family}, pixelSize, options);
  }

  // The shell's UI font chain.
  //
  // Segoe UI is not redistributable, so it cannot be shipped; it is listed
  // first so that a user who has legally installed it (from their own Windows
  // licence) gets exact metrics automatically. Selawik is Microsoft's
  // MIT-licensed, metric-compatible substitute and is the intended default.
  // DejaVu Sans is only a legibility backstop -- it is metrically wrong and
  // pixel-diff tests are expected to fail against it.
  static const std::vector<std::string>& uiFontChain();

  FT_Library ft() const { return ft_; }

 private:
  std::optional<std::string> resolveFamilyFile(const std::string& family) const;

  FT_Library ft_ = nullptr;
  bool lcdFilterAvailable_ = false;
  // Faces are keyed by file+size so repeated open() calls share one FT_Face.
  std::unordered_map<std::string, std::weak_ptr<Font>> cache_;
};

}  // namespace finux::text

// Exercises the text stack end to end: resolve a face, shape a string,
// rasterise it and composite it into a surface.

#include <string>

#include "check.hpp"
#include "gfx/image.hpp"
#include "text/font.hpp"
#include "text/text_renderer.hpp"

using namespace finux;

namespace {

int countInkPixels(const gfx::Image& image, Color background) {
  const uint32_t bg = gfx::premultiply(background);
  int ink = 0;
  for (int y = 0; y < image.height(); ++y) {
    const uint32_t* row = image.rowAt(y);
    for (int x = 0; x < image.width(); ++x) {
      if (row[x] != bg) ++ink;
    }
  }
  return ink;
}

}  // namespace

int main() {
  text::FontLibrary library;
  if (!library.valid()) {
    return finux::check::skip("text", "FreeType failed to initialise");
  }

  // Grayscale keeps the test independent of whether this FreeType build has
  // subpixel rendering compiled in.
  text::FontOptions options;
  options.antialias = text::Antialias::Grayscale;

  std::shared_ptr<text::Font> font =
      library.open(text::FontLibrary::uiFontChain(), 12, options);
  if (!font) {
    return finux::check::skip(
        "text", "no font from the UI chain is installed (need Selawik, "
                "Segoe UI or DejaVu Sans)");
  }

  text::TextRenderer renderer(library);

  // --- Metrics -------------------------------------------------------------
  CHECK(font->ascent() > 0);
  CHECK(font->descent() >= 0);
  CHECK(font->lineHeight() >= font->ascent());

  // --- Shaping -------------------------------------------------------------
  const text::ShapedText shaped = renderer.shape(*font, "Task View");
  CHECK(!shaped.glyphs.empty());
  CHECK_MSG(shaped.width > 0, "shaped run has no advance");

  // Width must grow with content, and an empty string must measure zero.
  CHECK_EQ(renderer.measure(*font, ""), 0);
  CHECK(renderer.measure(*font, "WW") > renderer.measure(*font, "W"));

  // --- Rasterisation -------------------------------------------------------
  const Color background = Color::rgb(0x1F1F1F);
  gfx::Image surface(200, 40);
  surface.fill(background);

  renderer.drawInBox(surface, {0, 0, 200, 40}, *font, "File Explorer",
                     Color::rgb(0xFFFFFF));
  const int ink = countInkPixels(surface, background);
  CHECK_MSG(ink > 0, "drawing text produced no pixels");

  // Text must stay inside the box it was given; a widget that bleeds into its
  // neighbour is the exact failure clipping exists to prevent.
  gfx::Image clipped(200, 40);
  clipped.fill(background);
  renderer.drawInBox(clipped, {0, 10, 60, 20}, *font,
                     "a very long label that cannot possibly fit",
                     Color::rgb(0xFFFFFF));
  for (int y = 0; y < 40; ++y) {
    if (y >= 10 && y < 30) continue;
    const uint32_t* row = clipped.rowAt(y);
    for (int x = 0; x < 200; ++x) {
      CHECK_MSG(row[x] == gfx::premultiply(background),
                "text escaped its box at row " + std::to_string(y));
      if (finux::check::failures > 0) break;
    }
    if (finux::check::failures > 0) break;
  }

  // --- Ellipsis ------------------------------------------------------------
  const std::string full = "Untitled - Notepad";
  const int fullWidth = renderer.measure(*font, full);
  CHECK(renderer.ellipsize(*font, full, fullWidth + 10) == full);

  const std::string trimmed = renderer.ellipsize(*font, full, fullWidth / 2);
  CHECK_MSG(trimmed != full, "ellipsize did not trim an overlong label");
  CHECK_MSG(renderer.measure(*font, trimmed) <= fullWidth / 2,
            "ellipsized label still overflows: " + trimmed);
  CHECK_MSG(trimmed.size() >= 3 && trimmed.compare(trimmed.size() - 3, 3,
                                                   "\xE2\x80\xA6") == 0,
            "ellipsized label does not end in U+2026: " + trimmed);

  // Trimming must cut whole code points, never split a UTF-8 sequence.
  const std::string unicode = "Café Übersicht Ausführen";
  for (int width = 4; width < 120; width += 3) {
    const std::string cut = renderer.ellipsize(*font, unicode, width);
    for (size_t i = 0; i < cut.size();) {
      const auto lead = static_cast<unsigned char>(cut[i]);
      size_t length = 1;
      if ((lead & 0xE0) == 0xC0) length = 2;
      else if ((lead & 0xF0) == 0xE0) length = 3;
      else if ((lead & 0xF8) == 0xF0) length = 4;
      CHECK_MSG(i + length <= cut.size(),
                "ellipsize split a UTF-8 sequence at width " +
                    std::to_string(width));
      i += length;
    }
    if (finux::check::failures > 0) break;
  }

  return finux::check::finish("text");
}

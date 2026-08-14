// Regression test for the interleaving of Canvas drawing and direct pixel
// access.
//
// Text rasterisation bypasses Blend2D and writes to the surface itself, which
// means the paint context has to be stood down and brought back. An earlier
// version stood it down permanently: every fill issued after the first text
// draw was silently discarded. Nothing crashed and nothing warned -- widgets
// simply stopped painting partway through the frame.
//
// The taskbar test did not catch it, because it only asserted the first task
// button, which is drawn before any text exists. These checks assert the
// *second* operation of each interleaved pair, which is where the failure was.

#include <string>

#include "check.hpp"
#include "core/color.hpp"
#include "gfx/canvas.hpp"
#include "gfx/image.hpp"
#include "text/font.hpp"
#include "text/text_renderer.hpp"

using namespace finux;

namespace {

const Color kBackground = Color::rgb(0x1F1F1F);
const Color kMarker = Color::rgb(0x0078D7);

bool isColor(const gfx::Image& image, int x, int y, Color c) {
  return image.rowAt(y)[x] == gfx::premultiply(c);
}

// A fill issued after syncPixels() must land.
void fillAfterSyncLands() {
  gfx::Image surface(64, 32);
  gfx::Canvas canvas(surface);
  canvas.clear(kBackground);

  canvas.fillRect({0, 0, 10, 10}, kMarker);
  canvas.syncPixels();  // as a glyph blit would
  canvas.fillRect({20, 0, 10, 10}, kMarker);
  canvas.flush();

  CHECK_MSG(isColor(surface, 5, 5, kMarker), "fill before syncPixels was lost");
  CHECK_MSG(isColor(surface, 25, 5, kMarker),
            "fill after syncPixels was silently discarded");
}

// Repeated suspend/revive cycles must keep working, not just the first.
void manyCyclesKeepWorking() {
  gfx::Image surface(200, 16);
  gfx::Canvas canvas(surface);
  canvas.clear(kBackground);

  for (int i = 0; i < 10; ++i) {
    canvas.syncPixels();
    canvas.fillRect({i * 20, 0, 10, 16}, kMarker);
  }
  canvas.flush();

  for (int i = 0; i < 10; ++i) {
    CHECK_MSG(isColor(surface, i * 20 + 5, 8, kMarker),
              "fill " + std::to_string(i) + " after a sync cycle was lost");
  }
}

// The clip stack must survive a suspend/revive, or a revived context would
// paint over its neighbours.
void clipSurvivesSync() {
  gfx::Image surface(100, 20);
  gfx::Canvas canvas(surface);
  canvas.clear(kBackground);

  canvas.pushClip({10, 0, 20, 20});
  canvas.syncPixels();
  // Ask to fill well beyond the clip; only the clipped part may appear.
  canvas.fillRect({0, 0, 100, 20}, kMarker);
  canvas.popClip();
  canvas.flush();

  CHECK_MSG(isColor(surface, 15, 10, kMarker), "clipped fill did not draw");
  CHECK_MSG(isColor(surface, 5, 10, kBackground),
            "fill escaped the clip to the left after syncPixels");
  CHECK_MSG(isColor(surface, 50, 10, kBackground),
            "fill escaped the clip to the right after syncPixels");
}

// A clip pushed while suspended must still take effect once revived.
void clipPushedWhileSuspendedApplies() {
  gfx::Image surface(100, 20);
  gfx::Canvas canvas(surface);
  canvas.clear(kBackground);

  canvas.syncPixels();
  canvas.pushClip({40, 0, 20, 20});
  canvas.fillRect({0, 0, 100, 20}, kMarker);
  canvas.popClip();
  canvas.flush();

  CHECK_MSG(isColor(surface, 45, 10, kMarker), "clipped fill did not draw");
  CHECK_MSG(isColor(surface, 10, 10, kBackground),
            "clip pushed while suspended was ignored on revive");
}

// The real scenario: fill, real text, fill. This is the shape of every widget
// that draws a background, a label, then anything else.
void fillTextFillWithRealGlyphs(text::FontLibrary& fonts, text::Font& font) {
  gfx::Image surface(300, 40);
  text::TextRenderer renderer(fonts);

  gfx::Canvas canvas(surface);
  canvas.clear(kBackground);

  canvas.fillRect({0, 0, 20, 40}, kMarker);

  canvas.syncPixels();
  renderer.drawInBox(surface, {30, 0, 200, 40}, font, "Untitled - Notepad",
                     Color::rgb(0xFFFFFF));

  // Everything below is issued after the glyph blit.
  canvas.fillRect({250, 0, 20, 40}, kMarker);
  canvas.fillRect({275, 37, 20, 3}, kMarker);
  canvas.flush();

  CHECK_MSG(isColor(surface, 10, 20, kMarker), "fill before text was lost");
  CHECK_MSG(isColor(surface, 260, 20, kMarker),
            "fill after real text rendering was discarded");
  CHECK_MSG(isColor(surface, 285, 38, kMarker),
            "second fill after real text rendering was discarded");

  bool ink = false;
  for (int y = 0; y < 40 && !ink; ++y) {
    for (int x = 30; x < 230; ++x) {
      if (!isColor(surface, x, y, kBackground)) {
        ink = true;
        break;
      }
    }
  }
  CHECK_MSG(ink, "text drew nothing, so the interleaving was not exercised");
}

}  // namespace

int main() {
  fillAfterSyncLands();
  manyCyclesKeepWorking();
  clipSurvivesSync();
  clipPushedWhileSuspendedApplies();

  text::FontLibrary fonts;
  if (fonts.valid()) {
    text::FontOptions options;
    options.antialias = text::Antialias::Grayscale;
    if (std::shared_ptr<text::Font> font =
            fonts.open(text::FontLibrary::uiFontChain(), 12, options)) {
      fillTextFillWithRealGlyphs(fonts, *font);
    } else {
      std::printf("note: no UI font; skipped the real-glyph interleave case\n");
    }
  }

  return finux::check::finish("canvas_interleave");
}

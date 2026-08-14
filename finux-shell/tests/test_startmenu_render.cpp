// Renders the Start menu offscreen and checks its three-column structure.
//
// The tile grid is the part most worth pinning down: the packer decides where
// every tile lands, and an off-by-one in the cell arithmetic produces a menu
// that still looks plausible in isolation while matching no Windows layout at
// all. These checks assert the arithmetic directly rather than by eye.

#include <string>
#include <vector>

#include "check.hpp"
#include "gfx/canvas.hpp"
#include "gfx/image.hpp"
#include "pixdiff/pixdiff.hpp"
#include "shell/startmenu.hpp"
#include "text/font.hpp"
#include "text/text_renderer.hpp"
#include "ui/theme.hpp"

using namespace finux;

namespace {

int countColor(const gfx::Image& image, Rect region, Color color) {
  const Rect clipped = region.intersected(image.bounds());
  const uint32_t want = gfx::premultiply(color);
  int count = 0;
  for (int y = clipped.y; y < clipped.bottom(); ++y) {
    const uint32_t* row = image.rowAt(y);
    for (int x = clipped.x; x < clipped.right(); ++x) {
      if (row[x] == want) ++count;
    }
  }
  return count;
}

int countNot(const gfx::Image& image, Rect region, Color color) {
  const Rect clipped = region.intersected(image.bounds());
  return clipped.w * clipped.h - countColor(image, clipped, color);
}

}  // namespace

int main() {
  text::FontLibrary fonts;
  if (!fonts.valid()) {
    return finux::check::skip("startmenu_render", "FreeType failed to init");
  }

  const ui::Theme theme = ui::Theme::win10();
  const ui::Metrics& m = theme.metrics;

  text::FontOptions options;
  options.antialias = text::Antialias::Grayscale;
  std::shared_ptr<text::Font> font =
      fonts.open(text::FontLibrary::uiFontChain(),
                 theme.scaled(m.uiFontPixelSize), options);
  if (!font) {
    return finux::check::skip("startmenu_render", "no usable UI font");
  }

  // --- grid arithmetic -----------------------------------------------------
  //
  // Three medium tiles must exactly fill the six-cell grid: if the gutters are
  // counted wrong the third tile wraps, which is the single most visible way a
  // tile layout can be wrong.
  CHECK_EQ(m.tileSpan(2) * 3 + m.tileGap * 2, m.tileGridWidth());
  CHECK_EQ(m.tileSpan(1), m.tileCell);
  CHECK_EQ(m.tileSpan(6), m.tileGridWidth());
  CHECK_EQ(shell::tileCells(shell::TileSize::Medium).w, 2);
  CHECK_EQ(shell::tileCells(shell::TileSize::Wide).w, 4);
  CHECK_EQ(m.startMenuWidth(), m.startMenuRailWidth + m.startMenuAppListWidth +
                                   m.startMenuTilePanelWidth());

  // --- render --------------------------------------------------------------
  const int width = theme.scaled(m.startMenuWidth());
  const int height = theme.scaled(m.startMenuHeight);

  gfx::Image surface(width, height);
  CHECK(surface.valid());

  shell::StartMenu menu(theme);
  menu.setBounds({0, 0, width, height});

  text::TextRenderer renderer(fonts);
  {
    gfx::Canvas canvas(surface);
    canvas.clear(Color::rgb(0xFF00FF));  // magenta: any gap shows immediately
    ui::PaintContext ctx{canvas, renderer, theme, *font, Point{0, 0}};
    menu.paintTree(ctx);
  }

  // The menu must cover its whole rect; no backdrop may show through.
  CHECK_MSG(countColor(surface, surface.bounds(), Color::rgb(0xFF00FF)) == 0,
            "the Start menu left gaps in its own bounds");

  // --- three columns -------------------------------------------------------
  const int railWidth = theme.scaled(m.startMenuRailWidth);
  const int listWidth = theme.scaled(m.startMenuAppListWidth);

  // Sample away from the border hairline and the glyphs.
  CHECK_MSG(surface.rowAt(height / 2)[2] ==
                gfx::premultiply(theme.palette.startMenuRail),
            "rail column is not painted with the rail colour");
  CHECK_MSG(surface.rowAt(2)[railWidth + listWidth / 2] ==
                gfx::premultiply(theme.palette.startMenuBackground),
            "app list column is not painted with the menu background");
  CHECK_MSG(surface.rowAt(height - 3)[railWidth + listWidth + 4] ==
                gfx::premultiply(theme.palette.startMenuTilePanel),
            "tile panel column is not painted with the panel colour");

  // Each column must actually draw content, not just its background.
  CHECK_MSG(countNot(surface, {0, 0, railWidth, height},
                     theme.palette.startMenuRail) > 0,
            "rail drew no icons");
  CHECK_MSG(countNot(surface, {railWidth, 0, listWidth, height},
                     theme.palette.startMenuBackground) > 0,
            "app list drew no entries");
  CHECK_MSG(countNot(surface, {railWidth + listWidth, 0,
                               width - railWidth - listWidth, height},
                     theme.palette.startMenuTilePanel) > 0,
            "tile panel drew no tiles");

  // --- tiles ---------------------------------------------------------------
  const int padding = theme.scaled(m.startMenuTilePadding);
  const int cell = theme.scaled(m.tileCell);
  const int gap = theme.scaled(m.tileGap);
  const int tileX = railWidth + listWidth + padding;

  // The first group's first row holds three medium tiles across. Verify the
  // third one starts where the arithmetic says it must, and that the gutters
  // between them are panel-coloured rather than filled.
  const int medium = cell * 2 + gap;
  const int thirdTileX = tileX + (medium + gap) * 2;
  CHECK_MSG(thirdTileX + medium <= width - padding,
            "three medium tiles do not fit the panel width");

  // Find the first tile row by looking for filled pixels that PERSIST for at
  // least a tile's height. A single filled row is the menu's own 1px border,
  // not a tile -- requiring persistence distinguishes them without the test
  // re-deriving where the layout put things.
  int inkRow = -1;
  int run = 0;
  for (int y = 0; y < height; ++y) {
    const bool filled = countNot(surface, {tileX, y, medium, 1},
                                 theme.palette.startMenuTilePanel) > medium / 2;
    run = filled ? run + 1 : 0;
    if (run >= cell) {
      inkRow = y - run + 1;
      break;
    }
  }
  CHECK_MSG(inkRow >= 0, "no tile row found in the panel");

  if (inkRow >= 0) {
    const int sampleY = inkRow + cell / 2;
    // Gutter between the first and second tile stays panel-coloured.
    const int gutterX = tileX + medium + gap / 2;
    CHECK_MSG(surface.rowAt(sampleY)[gutterX] ==
                  gfx::premultiply(theme.palette.startMenuTilePanel),
              "tiles bleed into the gutter between them");
    // The third tile is painted.
    CHECK_MSG(surface.rowAt(sampleY)[thirdTileX + medium / 2] !=
                  gfx::premultiply(theme.palette.startMenuTilePanel),
              "third medium tile in the row is missing");
  }

  // preferredHeight must describe the content actually drawn.
  CHECK_MSG(menu.preferredHeight(theme) > 0, "preferredHeight is zero");

  // --- fidelity ------------------------------------------------------------
  const pixdiff::FileResult fidelity = pixdiff::compareToReference(
      surface, std::string(FINUX_REFERENCE_DIR), "startmenu");
  if (!fidelity.referenceExisted) {
    std::printf("note: %s\n", fidelity.result.message.c_str());
  } else {
    CHECK_MSG(fidelity.result.passed, fidelity.result.message);
  }

  return finux::check::finish("startmenu_render");
}

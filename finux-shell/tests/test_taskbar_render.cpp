// Renders the taskbar to an offscreen surface -- no X server involved -- and
// checks both structural invariants and, when a reference capture is present,
// pixel fidelity against real Windows 10.
//
// Positions come from the bar's own layout rather than being recomputed here.
// An earlier version derived button positions from metrics, which made it a
// second implementation of layout: when the real one gained a search box and
// pinned buttons, the test kept checking the coordinates it had invented.

#include <string>
#include <vector>

#include "check.hpp"
#include "gfx/canvas.hpp"
#include "gfx/image.hpp"
#include "pixdiff/pixdiff.hpp"
#include "shell/taskbar.hpp"
#include "text/font.hpp"
#include "text/text_renderer.hpp"
#include "ui/theme.hpp"

using namespace finux;

namespace {

constexpr int kBarWidth = 1800;

int countMatching(const gfx::Image& image, Rect region,
                  bool (*predicate)(uint32_t)) {
  const Rect clipped = region.intersected(image.bounds());
  int count = 0;
  for (int y = clipped.y; y < clipped.bottom(); ++y) {
    const uint32_t* row = image.rowAt(y);
    for (int x = clipped.x; x < clipped.right(); ++x) {
      if (predicate(row[x])) ++count;
    }
  }
  return count;
}

// Blue-dominant, i.e. the Windows accent rather than any shell surface.
bool isAccentish(uint32_t px) {
  const uint32_t b = px & 0xFF;
  const uint32_t g = (px >> 8) & 0xFF;
  const uint32_t r = (px >> 16) & 0xFF;
  return b > 0x90 && b > r + 0x40 && g > r;
}

int countNonBackground(const gfx::Image& image, Rect region, Color background) {
  const Rect clipped = region.intersected(image.bounds());
  const uint32_t bg = gfx::premultiply(background);
  int count = 0;
  for (int y = clipped.y; y < clipped.bottom(); ++y) {
    const uint32_t* row = image.rowAt(y);
    for (int x = clipped.x; x < clipped.right(); ++x) {
      if (row[x] != bg) ++count;
    }
  }
  return count;
}

std::vector<platform::Toplevel> sampleWindows() {
  return {
      {1, "Untitled - Notepad", "Notepad", false, true, 0},
      {2, "Documents", "Explorer", false, false, 0},
      {3, "finux-shell - Visual Studio Code", "Code", true, false, 0},
  };
}

}  // namespace

int main() {
  text::FontLibrary fonts;
  if (!fonts.valid()) {
    return finux::check::skip("taskbar_render", "FreeType failed to initialise");
  }

  ui::Theme theme = ui::Theme::win10();  // light, as Windows ships
  // Transparency off: acrylic blurs whatever is behind the surface, which
  // makes exact colour assertions meaningless. The acrylic path has its own
  // coverage in test_effects and in the translucency check below.
  theme.transparencyEffects = false;

  // Grayscale so the result is reproducible regardless of whether this
  // FreeType has subpixel rendering; the shipped shell uses ClearType-alike.
  text::FontOptions options;
  options.antialias = text::Antialias::Grayscale;
  std::shared_ptr<text::Font> font = fonts.open(
      text::FontLibrary::uiFontChain(),
      theme.scaled(theme.metrics.uiFontPixelSize), options);
  if (!font) {
    return finux::check::skip("taskbar_render", "no usable UI font installed");
  }

  text::TextRenderer renderer(fonts);

  const int barHeight = theme.taskbarHeight();
  CHECK_EQ(barHeight, 40);

  gfx::Image surface(kBarWidth, barHeight);
  CHECK(surface.valid());

  shell::Taskbar taskbar(theme);
  taskbar.setBounds({0, 0, kBarWidth, barHeight});
  taskbar.setToplevels(sampleWindows());
  taskbar.setClock("4:44 PM", "6/25/2020");

  {
    gfx::Canvas canvas(surface);
    canvas.clear(theme.palette.taskbarBackground);
    ui::PaintContext ctx{canvas, renderer, theme, *font, Point{0, 0}};
    taskbar.paintTree(ctx);
  }

  const Color background = theme.palette.taskbarBackground;

  // --- left cluster --------------------------------------------------------
  const shell::StartButton* start = taskbar.startButton();
  CHECK(start != nullptr);
  if (start) {
    // The Start button carries the accent colour in the light theme, which is
    // the most distinctive thing about a stock Windows 10 taskbar.
    CHECK_MSG(countMatching(surface, start->bounds(), isAccentish) >
                  start->bounds().w * start->bounds().h / 2,
              "start button is not accent-filled");
    // A white glyph must sit on top of it.
    const uint32_t white = gfx::premultiply(Color::rgb(0xFFFFFF));
    CHECK_MSG(countMatching(surface, start->bounds(),
                            +[](uint32_t px) {
                              return px == gfx::premultiply(Color::rgb(0xFFFFFF));
                            }) > 0,
              "start button glyph is missing");
    (void)white;
  }

  const shell::SearchBox* search = taskbar.searchBox();
  CHECK(search != nullptr);
  if (search) {
    const Rect box = search->bounds();
    CHECK_MSG(box.h < barHeight,
              "search box should be inset from the taskbar's edges");
    // Its interior is white, unlike the surrounding bar.
    CHECK_MSG(surface.rowAt(box.y + box.h / 2)[box.x + box.w / 2] ==
                  gfx::premultiply(theme.palette.searchBoxBackground),
              "search box interior is not filled");
  }

  // --- task buttons --------------------------------------------------------
  //
  // EVERY running window carries an indicator along its bottom edge, not just
  // the active one. Checking only the first button is what once let a bug
  // through where everything drawn after the first label silently vanished.
  const auto& buttons = taskbar.taskButtons();
  CHECK_MSG(!buttons.empty(), "no task buttons were created");

  const int indicatorHeight = theme.metrics.runningIndicatorHeight;
  for (size_t i = 0; i < buttons.size(); ++i) {
    const Rect box = buttons[i]->bounds();
    const std::string which = "task button " + std::to_string(i);
    CHECK_MSG(!box.empty(), which + " has empty bounds");

    const Rect strip{box.x, barHeight - indicatorHeight, box.w, indicatorHeight};
    CHECK_MSG(countMatching(surface, strip, isAccentish) > 0,
              "no accent running indicator under " + which);

    // The indicator is inset horizontally; the button's outer edge must not
    // carry it, or adjacent buttons' indicators would visually merge.
    CHECK_MSG(countMatching(surface, {box.x, strip.y, 2, indicatorHeight},
                            isAccentish) == 0,
              "running indicator is not inset from the left edge of " + which);

    // The icon tile is a fill sitting between two text draws, so it is the
    // most sensitive thing in the frame to a suspended paint context.
    CHECK_MSG(countNonBackground(surface, box, background) > 0,
              "icon missing on " + which);
  }

  // Buttons must not overlap; adjacent ones sharing a pixel column would make
  // hit testing ambiguous.
  for (size_t i = 1; i < buttons.size(); ++i) {
    CHECK_MSG(buttons[i - 1]->bounds().right() <= buttons[i]->bounds().x,
              "task buttons " + std::to_string(i - 1) + " and " +
                  std::to_string(i) + " overlap");
  }

  // --- right cluster -------------------------------------------------------
  const shell::Clock* clock = taskbar.clockWidget();
  CHECK(clock != nullptr);
  if (clock) {
    CHECK_MSG(countNonBackground(surface, clock->bounds(), background) > 0,
              "clock drew nothing");
    CHECK_MSG(clock->bounds().right() <= kBarWidth,
              "clock overflows the taskbar");
  }

  const shell::SystemTray* tray = taskbar.systemTray();
  CHECK(tray != nullptr);
  if (tray && clock) {
    CHECK_MSG(tray->bounds().right() <= clock->bounds().x,
              "system tray overlaps the clock");
    CHECK_MSG(countNonBackground(surface, tray->bounds(), background) > 0,
              "system tray drew no icons");
  }

  // Nothing may paint over the show-desktop sliver at the far right edge.
  CHECK_MSG(countNonBackground(surface,
                               {kBarWidth - theme.metrics.showDesktopWidth + 1,
                                2, theme.metrics.showDesktopWidth - 2,
                                barHeight - 4},
                               background) == 0,
            "something painted over the show-desktop region");

  // Every pixel must be opaque: the bar is composited by the X server with no
  // alpha, so a stray translucent pixel would show as a hole.
  for (int y = 0; y < barHeight; ++y) {
    const uint32_t* row = surface.rowAt(y);
    for (int x = 0; x < kBarWidth; ++x) {
      if ((row[x] >> 24) != 0xFF) {
        CHECK_MSG(false, "non-opaque pixel at " + std::to_string(x) + "," +
                             std::to_string(y));
        break;
      }
    }
    if (finux::check::failures > 0) break;
  }

  // --- Fidelity ------------------------------------------------------------
  //
  // References are user-supplied captures of real Windows 10 (they cannot be
  // redistributed here). Their absence is reported, never silently passed.
  const pixdiff::FileResult fidelity = pixdiff::compareToReference(
      surface, std::string(FINUX_REFERENCE_DIR), "taskbar_1800x40");

  if (!fidelity.referenceExisted) {
    std::printf("note: %s\n", fidelity.result.message.c_str());
  } else {
    CHECK_MSG(fidelity.result.passed,
              fidelity.result.message +
                  (fidelity.diffPath.empty()
                       ? std::string()
                       : ("; diff written to " + fidelity.diffPath)));
  }

  return finux::check::finish("taskbar_render");
}

// Renders the taskbar to an offscreen surface -- no X server involved -- and
// checks both structural invariants and, when a reference capture is present,
// pixel fidelity against real Windows 10.

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

constexpr int kBarWidth = 1280;

bool colorAt(const gfx::Image& image, int x, int y, Color expected) {
  return image.rowAt(y)[x] == gfx::premultiply(expected);
}

// Counts pixels whose colour is dominated by the accent's blue, which is how
// the running indicator is identified without hardcoding its position twice.
int countAccentish(const gfx::Image& image, Rect region) {
  int count = 0;
  for (int y = region.y; y < region.bottom(); ++y) {
    const uint32_t* row = image.rowAt(y);
    for (int x = region.x; x < region.right(); ++x) {
      const uint32_t b = row[x] & 0xFF;
      const uint32_t g = (row[x] >> 8) & 0xFF;
      const uint32_t r = (row[x] >> 16) & 0xFF;
      if (b > 0x90 && b > r + 0x40 && g > r) ++count;
    }
  }
  return count;
}

int countNonBackground(const gfx::Image& image, Rect region, Color background) {
  const uint32_t bg = gfx::premultiply(background);
  int count = 0;
  for (int y = region.y; y < region.bottom(); ++y) {
    const uint32_t* row = image.rowAt(y);
    for (int x = region.x; x < region.right(); ++x) {
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

  const ui::Theme theme = ui::Theme::win10();

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
  taskbar.setClock("4:17 PM", "8/14/2026");

  {
    gfx::Canvas canvas(surface);
    canvas.clear(theme.palette.taskbarBackground);
    ui::PaintContext ctx{canvas, renderer, theme, *font, Point{0, 0}};
    taskbar.paintTree(ctx);
  }

  // --- Structure -----------------------------------------------------------

  // Empty stretch between the last task button and the clock stays background.
  CHECK_MSG(colorAt(surface, 900, 4, theme.palette.taskbarBackground),
            "expected bare taskbar background in the empty region");

  // The start button occupies the leftmost 48px and draws something.
  const int startWidth = theme.metrics.startButtonWidth;
  CHECK_MSG(countNonBackground(surface, {0, 0, startWidth, barHeight},
                               theme.palette.taskbarBackground) > 0,
            "start button drew nothing");

  // Task buttons begin immediately after the start button. EVERY running
  // window carries an indicator along its bottom edge, not just the active
  // one -- checking only the first button is what let a bug through where
  // everything drawn after the first label silently vanished.
  const int buttonWidth = theme.metrics.taskButtonWidth;
  const int indicatorHeight = theme.metrics.runningIndicatorHeight;
  const int windowCount = static_cast<int>(sampleWindows().size());

  for (int i = 0; i < windowCount; ++i) {
    const Rect button{startWidth + i * buttonWidth, 0, buttonWidth, barHeight};
    const std::string which = "task button " + std::to_string(i);

    const Rect indicatorStrip{button.x, barHeight - indicatorHeight, button.w,
                              indicatorHeight};
    CHECK_MSG(countAccentish(surface, indicatorStrip) > 0,
              "no accent running indicator under " + which);

    // The indicator is inset horizontally; the button's outer edge must not
    // carry it, or adjacent buttons' indicators would visually merge.
    CHECK_MSG(
        countAccentish(surface, {button.x, indicatorStrip.y, 2,
                                 indicatorHeight}) == 0,
        "running indicator is not inset from the left edge of " + which);

    // The icon tile is a Blend2D fill sitting between two text draws, so it is
    // the most sensitive thing in the frame to a suspended paint context.
    const int iconSize = theme.metrics.taskbarIconSize;
    const Rect iconRect{button.x + 8, (barHeight - iconSize) / 2, iconSize,
                        iconSize};
    CHECK_MSG(countNonBackground(surface, iconRect,
                                 theme.palette.taskbarBackground) >
                  iconSize * iconSize / 2,
              "icon tile missing or mostly unpainted on " + which);
  }

  // The clock sits at the right, clear of the show-desktop sliver.
  const Rect clockRegion{kBarWidth - 200, 0, 200 - theme.metrics.showDesktopWidth,
                         barHeight};
  CHECK_MSG(countNonBackground(surface, clockRegion,
                               theme.palette.taskbarBackground) > 0,
            "clock drew nothing");

  // Nothing may paint over the show-desktop sliver at the far right.
  CHECK_MSG(countNonBackground(
                surface,
                {kBarWidth - theme.metrics.showDesktopWidth, 0,
                 theme.metrics.showDesktopWidth, barHeight},
                theme.palette.taskbarBackground) == 0,
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
  const std::string referenceDir = std::string(FINUX_REFERENCE_DIR);
  const pixdiff::FileResult fidelity =
      pixdiff::compareToReference(surface, referenceDir, "taskbar_1280x40");

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

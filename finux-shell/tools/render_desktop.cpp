// Renders the whole shell -- backdrop, Start menu and taskbar -- to a single
// PNG, offscreen and deterministically.
//
// This is the artefact you hold next to a Windows 10 screenshot. Running the
// real shell needs an X server, a window manager and live client windows, and
// still cannot show the Start menu until the flyout is wired to the button.
// Rendering the composite directly sidesteps all of that, and because the
// clock is injected rather than read, two runs produce identical bytes.
//
//   finux-render-desktop [output.png] [width] [height] [light|dark]

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "gfx/canvas.hpp"
#include "gfx/image.hpp"
#include "shell/startmenu.hpp"
#include "shell/taskbar.hpp"
#include "text/font.hpp"
#include "text/text_renderer.hpp"
#include "ui/theme.hpp"

using namespace finux;

namespace {

// A blue gradient standing in for the Windows 10 desktop background. The
// shipped wallpaper is Microsoft artwork and is not redistributable; this is
// only here so the shell's translucent edges have something to sit against.
void paintBackdrop(gfx::Canvas& canvas, Rect screen) {
  canvas.fillLinearGradient(screen, {screen.x, screen.y},
                            {screen.right(), screen.bottom()},
                            Color::rgb(0x0A63B0), Color::rgb(0x1E8FE0));
  canvas.fillLinearGradient(
      {screen.x, screen.y, screen.w, screen.h / 2}, {screen.x, screen.y},
      {screen.x, screen.y + screen.h / 2}, Color::rgba(0x000000, 0x2A),
      Color::rgba(0x000000, 0x00));
}

std::vector<platform::Toplevel> sampleWindows() {
  return {
      {1, "Untitled - Notepad", "Notepad", false, true, 0},
      {2, "Documents", "Explorer", false, false, 0},
  };
}

}  // namespace

int main(int argc, char** argv) {
  const std::string output = argc > 1 ? argv[1] : "desktop.png";
  const int width = argc > 2 ? std::atoi(argv[2]) : 1800;
  const int height = argc > 3 ? std::atoi(argv[3]) : 900;
  const std::string mode = argc > 4 ? argv[4] : "light";

  if (width <= 0 || height <= 0) {
    std::fprintf(stderr, "render-desktop: bad dimensions\n");
    return 2;
  }

  text::FontLibrary fonts;
  if (!fonts.valid()) {
    std::fprintf(stderr, "render-desktop: FreeType failed to initialise\n");
    return 1;
  }

  const ui::Theme theme =
      (mode == "dark") ? ui::Theme::win10Dark() : ui::Theme::win10Light();

  std::shared_ptr<text::Font> uiFont = fonts.open(
      text::FontLibrary::uiFontChain(),
      theme.scaled(theme.metrics.uiFontPixelSize));
  if (!uiFont) {
    std::fprintf(stderr, "render-desktop: no usable UI font\n");
    return 1;
  }
  if (uiFont->family() != "Segoe UI" && uiFont->family() != "Selawik") {
    std::fprintf(stderr,
                 "render-desktop: warning: using '%s'; install Selawik for "
                 "correct metrics\n",
                 uiFont->family().c_str());
  }

  gfx::Image surface(width, height);
  if (!surface.valid()) {
    std::fprintf(stderr, "render-desktop: could not allocate the surface\n");
    return 1;
  }

  text::TextRenderer renderer(fonts);
  const Rect screen{0, 0, width, height};
  const int barHeight = theme.taskbarHeight();

  shell::Taskbar taskbar(theme);
  taskbar.setBounds({0, height - barHeight, width, barHeight});
  taskbar.setToplevels(sampleWindows());
  taskbar.setClock("4:44 PM", "6/25/2020");

  shell::StartMenu startMenu(theme);
  const int menuWidth = theme.scaled(theme.metrics.startMenuWidth());
  const int menuHeight =
      std::min(theme.scaled(theme.metrics.startMenuHeight), height - barHeight);
  // The menu is anchored to the bottom-left corner of the work area, so it
  // grows upward from the taskbar rather than downward from the screen top.
  startMenu.setBounds({0, height - barHeight - menuHeight, menuWidth,
                       menuHeight});

  {
    gfx::Canvas canvas(surface);
    paintBackdrop(canvas, screen);

    ui::PaintContext ctx{canvas, renderer, theme, *uiFont, Point{0, 0}};
    startMenu.paintTree(ctx);
    taskbar.paintTree(ctx);
  }

  if (!surface.savePng(output)) {
    std::fprintf(stderr, "render-desktop: could not write %s\n",
                 output.c_str());
    return 1;
  }
  std::printf("wrote %s (%dx%d, %s theme, font '%s')\n", output.c_str(), width,
              height, mode.c_str(), uiFont->family().c_str());
  return 0;
}

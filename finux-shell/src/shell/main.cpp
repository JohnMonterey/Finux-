#include <poll.h>

#include <cstdio>
#include <ctime>
#include <memory>
#include <string>

#include "gfx/canvas.hpp"
#include "gfx/image.hpp"
#include "platform/platform.hpp"
#include "shell/taskbar.hpp"
#include "text/font.hpp"
#include "text/text_renderer.hpp"
#include "ui/theme.hpp"

namespace {

using namespace finux;

struct ClockText {
  std::string time;
  std::string date;
};

// Windows' default short formats for an en-US install. Locale-driven
// formatting belongs here once the settings layer exists; hardcoding it now
// keeps the reference rendering deterministic.
ClockText currentClock() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);

  char time[32] = {};
  char date[32] = {};
  std::strftime(time, sizeof(time), "%l:%M %p", &local);
  std::strftime(date, sizeof(date), "%-m/%-d/%Y", &local);

  // %l pads to two characters with a leading space.
  const char* trimmed = time;
  while (*trimmed == ' ') ++trimmed;
  return {trimmed, date};
}

}  // namespace

int main() {
  std::unique_ptr<platform::Platform> plat = platform::Platform::createX11();
  if (!plat) {
    std::fprintf(stderr,
                 "finux-shell: cannot connect to an X11 display.\n"
                 "  Set DISPLAY, or run under Xvfb for a headless check.\n");
    return 1;
  }

  text::FontLibrary fonts;
  if (!fonts.valid()) {
    std::fprintf(stderr, "finux-shell: failed to initialise FreeType.\n");
    return 1;
  }

  const ui::Theme theme = ui::Theme::win10();

  std::shared_ptr<text::Font> uiFont = fonts.open(
      text::FontLibrary::uiFontChain(), theme.scaled(theme.metrics.uiFontPixelSize));
  if (!uiFont) {
    std::fprintf(stderr,
                 "finux-shell: no usable UI font.\n"
                 "  Install Selawik (metric-compatible with Segoe UI) or "
                 "supply Segoe UI itself.\n");
    return 1;
  }
  if (uiFont->family() != "Segoe UI" && uiFont->family() != "Selawik") {
    std::fprintf(stderr,
                 "finux-shell: warning: falling back to '%s'.\n"
                 "  Metrics will not match Windows and pixel-diff tests will "
                 "fail. Install Selawik.\n",
                 uiFont->family().c_str());
  }

  text::TextRenderer textRenderer(fonts);

  const Rect monitor = plat->primaryMonitor();
  const int barHeight = theme.taskbarHeight();
  const Rect barGeometry{monitor.x, monitor.bottom() - barHeight, monitor.w,
                         barHeight};

  std::unique_ptr<platform::Window> window =
      plat->createDockWindow(barGeometry, platform::DockEdge::Bottom);
  if (!window) {
    std::fprintf(stderr, "finux-shell: failed to create the dock window.\n");
    return 1;
  }

  shell::Taskbar taskbar(theme);
  taskbar.setBounds({0, 0, barGeometry.w, barGeometry.h});
  taskbar.onTaskActivated = [&plat](uint64_t id, ui::MouseButton button) {
    if (button == ui::MouseButton::Left) plat->activateToplevel(id);
  };
  taskbar.setToplevels(plat->toplevels());

  gfx::Image surface(barGeometry.w, barGeometry.h);
  if (!surface.valid()) {
    std::fprintf(stderr, "finux-shell: failed to allocate the bar surface.\n");
    return 1;
  }

  ClockText clock = currentClock();
  taskbar.setClock(clock.time, clock.date);

  const auto repaint = [&] {
    gfx::Canvas canvas(surface);
    canvas.clear(theme.palette.taskbarBackground);
    ui::PaintContext ctx{canvas,  textRenderer, theme,
                         *uiFont, Point{0, 0}};
    taskbar.paintTree(ctx);
    canvas.flush();
    window->present(surface);
  };

  repaint();

  // The clock only needs second-level accuracy, so the loop waits on the
  // display fd with a timeout rather than spinning or running a render thread.
  pollfd fds{plat->eventFd(), POLLIN, 0};
  bool running = true;
  while (running) {
    poll(&fds, 1, 1000);

    bool needsRepaint = false;
    platform::Event event;
    while (plat->pollEvent(event)) {
      switch (event.type) {
        case platform::Event::Type::Quit:
          running = false;
          break;
        case platform::Event::Type::Expose:
          needsRepaint = true;
          break;
        case platform::Event::Type::ToplevelsChanged:
          taskbar.setToplevels(plat->toplevels());
          needsRepaint = true;
          break;
        case platform::Event::Type::MouseMove:
          taskbar.dispatchMouseMove(event.position);
          break;
        case platform::Event::Type::MouseDown:
          taskbar.dispatchMouseDown(event.position, event.button);
          break;
        case platform::Event::Type::MouseUp:
          taskbar.dispatchMouseUp(event.position, event.button);
          break;
        case platform::Event::Type::MouseLeave:
          taskbar.dispatchMouseLeave();
          break;
        default:
          break;
      }
    }

    const ClockText now = currentClock();
    if (now.time != clock.time || now.date != clock.date) {
      clock = now;
      taskbar.setClock(clock.time, clock.date);
    }

    if (needsRepaint || taskbar.dirty()) repaint();
  }
  return 0;
}

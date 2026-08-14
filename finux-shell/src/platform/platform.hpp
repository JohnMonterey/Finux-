#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/geometry.hpp"
#include "gfx/image.hpp"
#include "ui/widget.hpp"

namespace finux::platform {

using ui::MouseButton;

// A window belonging to some other application, as the taskbar sees it.
struct Toplevel {
  uint64_t id = 0;
  std::string title;
  // WM_CLASS on X11 / app_id on Wayland. This is the key that ties a running
  // window to a pinned .desktop entry, so it must survive backend swaps.
  std::string appId;
  bool minimized = false;
  bool active = false;
  int workspace = -1;
};

enum class DockEdge { Top, Bottom, Left, Right };

struct Event {
  enum class Type {
    None,
    Expose,
    Resize,
    MouseMove,
    MouseDown,
    MouseUp,
    MouseLeave,
    // The window list changed: a window opened, closed, was renamed,
    // minimised or focused. The shell re-reads toplevels() rather than
    // receiving a diff, because the authoritative list lives in the WM.
    ToplevelsChanged,
    Quit,
  };

  Type type = Type::None;
  Point position;
  Size size;
  MouseButton button = MouseButton::Left;
};

// A surface owned by the shell.
class Window {
 public:
  virtual ~Window() = default;

  virtual Size size() const = 0;

  // Copies `image` to the screen. The image must match size().
  virtual void present(const gfx::Image& image) = 0;

  // Reserves `thickness` pixels along `edge` so maximised windows stop at the
  // taskbar instead of underneath it. This is the one capability that makes a
  // taskbar a shell component rather than a window that merely looks like one.
  virtual void reserveEdge(DockEdge edge, int thickness) = 0;
};

// Display-server abstraction.
//
// X11 is implemented first because EWMH exposes everything a Windows-style
// taskbar needs -- strut reservation, the client list, activation, and (via
// XComposite) live window thumbnails -- under any standard window manager.
//
// A Wayland backend fits the same interface via wlr-layer-shell,
// wlr-foreign-toplevel-management and wlr-screencopy, which restricts it to
// wlroots compositors and KWin. GNOME implements none of the three and
// therefore cannot host this shell at all; that is a protocol gap, not a
// porting task.
class Platform {
 public:
  virtual ~Platform() = default;

  static std::unique_ptr<Platform> createX11();

  virtual Rect primaryMonitor() const = 0;

  virtual std::unique_ptr<Window> createDockWindow(Rect geometry,
                                                   DockEdge edge) = 0;

  // Windows that belong in a taskbar: docks, desktops, splash screens and
  // anything asking to skip the taskbar are already filtered out.
  virtual std::vector<Toplevel> toplevels() = 0;

  virtual void activateToplevel(uint64_t id) = 0;
  virtual void minimizeToplevel(uint64_t id) = 0;

  // Non-blocking. Returns false when no event was pending.
  virtual bool pollEvent(Event& out) = 0;
  // Blocks until at least one event is available, then behaves like pollEvent.
  virtual bool waitEvent(Event& out) = 0;

  // File descriptor for the display connection, so the shell can select() on
  // it alongside timers (the clock) and D-Bus.
  virtual int eventFd() const = 0;
};

}  // namespace finux::platform

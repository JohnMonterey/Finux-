#pragma once

#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>

#include <string>
#include <vector>

#include "platform/platform.hpp"

namespace finux::platform {

class X11Platform;

class X11Window final : public Window {
 public:
  X11Window(X11Platform& platform, Rect geometry, DockEdge edge);
  ~X11Window() override;

  Size size() const override { return geometry_.size(); }
  void present(const gfx::Image& image) override;
  void reserveEdge(DockEdge edge, int thickness) override;

  xcb_window_t handle() const { return window_; }

 private:
  X11Platform& platform_;
  Rect geometry_;
  DockEdge edge_;
  xcb_window_t window_ = XCB_WINDOW_NONE;
  xcb_gcontext_t gc_ = 0;
};

class X11Platform final : public Platform {
 public:
  X11Platform() = default;
  ~X11Platform() override;

  bool open();

  Rect primaryMonitor() const override;
  std::unique_ptr<Window> createDockWindow(Rect geometry,
                                           DockEdge edge) override;
  std::vector<Toplevel> toplevels() override;
  void activateToplevel(uint64_t id) override;
  void minimizeToplevel(uint64_t id) override;
  bool pollEvent(Event& out) override;
  bool waitEvent(Event& out) override;
  int eventFd() const override;

  xcb_connection_t* connection() const { return conn_; }
  xcb_screen_t* screen() const { return screen_; }
  xcb_ewmh_connection_t* ewmh() { return &ewmh_; }

 private:
  bool translate(xcb_generic_event_t* raw, Event& out);
  std::string windowTitle(xcb_window_t window) const;
  std::string windowAppId(xcb_window_t window) const;
  bool shouldShowInTaskbar(xcb_window_t window) const;
  bool isMinimized(xcb_window_t window) const;

  xcb_connection_t* conn_ = nullptr;
  xcb_screen_t* screen_ = nullptr;
  int screenNumber_ = 0;
  xcb_ewmh_connection_t ewmh_{};
  bool ewmhReady_ = false;
  // ICCCM, not EWMH, so xcb_ewmh does not intern it for us.
  xcb_atom_t wmChangeState_ = XCB_ATOM_NONE;
  // Excluded from the task list: a taskbar must not list itself.
  xcb_window_t dockWindow_ = XCB_WINDOW_NONE;
};

}  // namespace finux::platform

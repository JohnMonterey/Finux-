#include "platform/x11/x11_platform.hpp"

#include <xcb/xcb_icccm.h>

#include <algorithm>
#include <cstring>

namespace finux::platform {
namespace {

MouseButton toMouseButton(xcb_button_t detail) {
  switch (detail) {
    case 2: return MouseButton::Middle;
    case 3: return MouseButton::Right;
    default: return MouseButton::Left;
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// X11Window
// ---------------------------------------------------------------------------

X11Window::X11Window(X11Platform& platform, Rect geometry, DockEdge edge)
    : platform_(platform), geometry_(geometry), edge_(edge) {
  xcb_connection_t* conn = platform_.connection();
  xcb_screen_t* screen = platform_.screen();

  window_ = xcb_generate_id(conn);

  const uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
  const uint32_t values[] = {
      screen->black_pixel,
      XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
          XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
          XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_LEAVE_WINDOW,
  };

  xcb_create_window(conn, XCB_COPY_FROM_PARENT, window_, screen->root,
                    static_cast<int16_t>(geometry.x),
                    static_cast<int16_t>(geometry.y),
                    static_cast<uint16_t>(geometry.w),
                    static_cast<uint16_t>(geometry.h), 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask,
                    values);

  gc_ = xcb_generate_id(conn);
  xcb_create_gc(conn, gc_, window_, 0, nullptr);

  xcb_ewmh_connection_t* ewmh = platform_.ewmh();

  // _NET_WM_WINDOW_TYPE_DOCK keeps the bar off the window list and above
  // ordinary windows without resorting to override-redirect, which would also
  // cost us keyboard focus and WM-managed stacking.
  xcb_ewmh_set_wm_window_type(ewmh, window_, 1, &ewmh->_NET_WM_WINDOW_TYPE_DOCK);

  xcb_atom_t states[] = {ewmh->_NET_WM_STATE_ABOVE, ewmh->_NET_WM_STATE_STICKY,
                         ewmh->_NET_WM_STATE_SKIP_TASKBAR,
                         ewmh->_NET_WM_STATE_SKIP_PAGER};
  xcb_ewmh_set_wm_state(ewmh, window_, 4, states);

  // Present on every workspace.
  xcb_ewmh_set_wm_desktop(ewmh, window_, 0xFFFFFFFF);

  const char* name = "finux-shell";
  xcb_ewmh_set_wm_name(ewmh, window_, std::strlen(name), name);
  xcb_icccm_set_wm_class(conn, window_, sizeof("finux-shell\0Finux-shell") - 1,
                         "finux-shell\0Finux-shell");

  // Without size hints the window manager considers our geometry merely a
  // suggestion and applies its own placement policy: openbox parks the bar at
  // the top of the screen while still honouring a bottom-edge strut, so the
  // reserved space and the bar end up on opposite edges.
  //
  // USPosition (rather than PPosition) states the position is a deliberate
  // choice rather than a default, which is what stops placement heuristics
  // from running at all. Pinning min == max size likewise keeps the WM from
  // resizing a bar whose height is a fixed Windows metric.
  xcb_size_hints_t hints{};
  xcb_icccm_size_hints_set_position(&hints, 1, geometry.x, geometry.y);
  xcb_icccm_size_hints_set_size(&hints, 1, geometry.w, geometry.h);
  xcb_icccm_size_hints_set_min_size(&hints, geometry.w, geometry.h);
  xcb_icccm_size_hints_set_max_size(&hints, geometry.w, geometry.h);
  xcb_icccm_set_wm_normal_hints(conn, window_, &hints);

  reserveEdge(edge_, (edge_ == DockEdge::Top || edge_ == DockEdge::Bottom)
                         ? geometry.h
                         : geometry.w);

  xcb_map_window(conn, window_);
  xcb_flush(conn);
}

X11Window::~X11Window() {
  xcb_connection_t* conn = platform_.connection();
  if (gc_) xcb_free_gc(conn, gc_);
  if (window_) xcb_destroy_window(conn, window_);
  xcb_flush(conn);
}

void X11Window::reserveEdge(DockEdge edge, int thickness) {
  edge_ = edge;
  xcb_ewmh_wm_strut_partial_t strut{};

  // The *_start/*_end fields bound the strut along the edge. Getting these
  // wrong is what makes a panel reserve space across every monitor instead of
  // just its own, so they are derived from the window geometry, not assumed.
  switch (edge) {
    case DockEdge::Top:
      strut.top = static_cast<uint32_t>(thickness);
      strut.top_start_x = static_cast<uint32_t>(geometry_.left());
      strut.top_end_x = static_cast<uint32_t>(geometry_.right() - 1);
      break;
    case DockEdge::Bottom:
      strut.bottom = static_cast<uint32_t>(thickness);
      strut.bottom_start_x = static_cast<uint32_t>(geometry_.left());
      strut.bottom_end_x = static_cast<uint32_t>(geometry_.right() - 1);
      break;
    case DockEdge::Left:
      strut.left = static_cast<uint32_t>(thickness);
      strut.left_start_y = static_cast<uint32_t>(geometry_.top());
      strut.left_end_y = static_cast<uint32_t>(geometry_.bottom() - 1);
      break;
    case DockEdge::Right:
      strut.right = static_cast<uint32_t>(thickness);
      strut.right_start_y = static_cast<uint32_t>(geometry_.top());
      strut.right_end_y = static_cast<uint32_t>(geometry_.bottom() - 1);
      break;
  }

  xcb_ewmh_connection_t* ewmh = platform_.ewmh();
  xcb_ewmh_set_wm_strut_partial(ewmh, window_, strut);
  // Older WMs only understand _NET_WM_STRUT; set both.
  xcb_ewmh_set_wm_strut(ewmh, window_, strut.left, strut.right, strut.top,
                        strut.bottom);
  xcb_flush(platform_.connection());
}

void X11Window::present(const gfx::Image& image) {
  if (!image.valid()) return;
  xcb_connection_t* conn = platform_.connection();

  const int width = std::min(image.width(), geometry_.w);
  const int height = std::min(image.height(), geometry_.h);
  if (width <= 0 || height <= 0) return;

  // Rows are uploaded in batches sized to the server's request limit. A
  // 1920x40 bar fits in a single request, but the shell must not silently
  // corrupt its own output on a server with a small maximum request length.
  const uint32_t maxRequestWords = xcb_get_maximum_request_length(conn);
  const size_t headerWords = 32;
  const size_t rowWords = static_cast<size_t>(width);  // 4 bytes per pixel
  const size_t budget =
      (maxRequestWords > headerWords) ? maxRequestWords - headerWords : rowWords;
  int rowsPerChunk = static_cast<int>(std::max<size_t>(1, budget / rowWords));
  rowsPerChunk = std::min(rowsPerChunk, height);

  std::vector<uint32_t> scratch(static_cast<size_t>(rowsPerChunk) * width);
  const int srcStride = image.strideWords();
  const uint32_t* src = image.pixels();
  if (!src) return;

  for (int y = 0; y < height; y += rowsPerChunk) {
    const int rows = std::min(rowsPerChunk, height - y);
    for (int r = 0; r < rows; ++r) {
      std::memcpy(scratch.data() + static_cast<size_t>(r) * width,
                  src + static_cast<ptrdiff_t>(y + r) * srcStride,
                  static_cast<size_t>(width) * sizeof(uint32_t));
    }
    xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, window_, gc_,
                  static_cast<uint16_t>(width), static_cast<uint16_t>(rows), 0,
                  static_cast<int16_t>(y), 0, platform_.screen()->root_depth,
                  static_cast<uint32_t>(rows * width * sizeof(uint32_t)),
                  reinterpret_cast<const uint8_t*>(scratch.data()));
  }
  xcb_flush(conn);
}

// ---------------------------------------------------------------------------
// X11Platform
// ---------------------------------------------------------------------------

X11Platform::~X11Platform() {
  if (ewmhReady_) xcb_ewmh_connection_wipe(&ewmh_);
  if (conn_) xcb_disconnect(conn_);
}

bool X11Platform::open() {
  int screenNumber = 0;
  conn_ = xcb_connect(nullptr, &screenNumber);
  if (!conn_ || xcb_connection_has_error(conn_)) {
    conn_ = nullptr;
    return false;
  }
  screenNumber_ = screenNumber;

  const xcb_setup_t* setup = xcb_get_setup(conn_);
  xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
  for (int i = 0; i < screenNumber && it.rem; ++i) xcb_screen_next(&it);
  screen_ = it.data;
  if (!screen_) return false;

  xcb_intern_atom_cookie_t* cookies = xcb_ewmh_init_atoms(conn_, &ewmh_);
  if (!xcb_ewmh_init_atoms_replies(&ewmh_, cookies, nullptr)) return false;
  ewmhReady_ = true;

  {
    const char* name = "WM_CHANGE_STATE";
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(
        conn_,
        xcb_intern_atom(conn_, 0, static_cast<uint16_t>(std::strlen(name)),
                        name),
        nullptr);
    if (reply) {
      wmChangeState_ = reply->atom;
      free(reply);
    }
  }

  // Watch the root for _NET_CLIENT_LIST / _NET_ACTIVE_WINDOW changes so the
  // task button list stays live without polling.
  const uint32_t rootMask = XCB_EVENT_MASK_PROPERTY_CHANGE;
  xcb_change_window_attributes(conn_, screen_->root, XCB_CW_EVENT_MASK,
                               &rootMask);
  xcb_flush(conn_);
  return true;
}

std::unique_ptr<Platform> Platform::createX11() {
  auto platform = std::make_unique<X11Platform>();
  if (!platform->open()) return nullptr;
  return platform;
}

Rect X11Platform::primaryMonitor() const {
  if (!screen_) return {};
  // RandR would give per-output geometry; for a single-head system the screen
  // dimensions are identical, and multi-monitor placement is a later stage.
  return {0, 0, screen_->width_in_pixels, screen_->height_in_pixels};
}

std::unique_ptr<Window> X11Platform::createDockWindow(Rect geometry,
                                                      DockEdge edge) {
  auto window = std::make_unique<X11Window>(*this, geometry, edge);
  dockWindow_ = window->handle();
  return window;
}

std::string X11Platform::windowTitle(xcb_window_t window) const {
  xcb_ewmh_get_utf8_strings_reply_t reply{};
  // _NET_WM_NAME (UTF-8) is authoritative; WM_NAME is the legacy fallback for
  // applications that never adopted EWMH.
  if (xcb_ewmh_get_wm_name_reply(
          const_cast<xcb_ewmh_connection_t*>(&ewmh_),
          xcb_ewmh_get_wm_name(const_cast<xcb_ewmh_connection_t*>(&ewmh_),
                               window),
          &reply, nullptr)) {
    std::string title(reply.strings, reply.strings_len);
    xcb_ewmh_get_utf8_strings_reply_wipe(&reply);
    return title;
  }

  xcb_icccm_get_text_property_reply_t legacy{};
  if (xcb_icccm_get_wm_name_reply(conn_, xcb_icccm_get_wm_name(conn_, window),
                                  &legacy, nullptr)) {
    std::string title(legacy.name, legacy.name_len);
    xcb_icccm_get_text_property_reply_wipe(&legacy);
    return title;
  }
  return {};
}

std::string X11Platform::windowAppId(xcb_window_t window) const {
  xcb_icccm_get_wm_class_reply_t reply{};
  if (!xcb_icccm_get_wm_class_reply(
          conn_, xcb_icccm_get_wm_class(conn_, window), &reply, nullptr)) {
    return {};
  }
  // The class (second field) is the stable identifier; the instance name
  // varies with argv[0]. Pinned-app matching keys off the class.
  std::string appId = reply.class_name ? reply.class_name : "";
  xcb_icccm_get_wm_class_reply_wipe(&reply);
  return appId;
}

bool X11Platform::shouldShowInTaskbar(xcb_window_t window) const {
  auto* ewmh = const_cast<xcb_ewmh_connection_t*>(&ewmh_);

  xcb_ewmh_get_atoms_reply_t types{};
  if (xcb_ewmh_get_wm_window_type_reply(
          ewmh, xcb_ewmh_get_wm_window_type(ewmh, window), &types, nullptr)) {
    bool excluded = false;
    for (unsigned i = 0; i < types.atoms_len; ++i) {
      const xcb_atom_t a = types.atoms[i];
      if (a == ewmh_._NET_WM_WINDOW_TYPE_DOCK ||
          a == ewmh_._NET_WM_WINDOW_TYPE_DESKTOP ||
          a == ewmh_._NET_WM_WINDOW_TYPE_TOOLBAR ||
          a == ewmh_._NET_WM_WINDOW_TYPE_MENU ||
          a == ewmh_._NET_WM_WINDOW_TYPE_SPLASH ||
          a == ewmh_._NET_WM_WINDOW_TYPE_UTILITY) {
        excluded = true;
        break;
      }
    }
    xcb_ewmh_get_atoms_reply_wipe(&types);
    if (excluded) return false;
  }

  xcb_ewmh_get_atoms_reply_t states{};
  if (xcb_ewmh_get_wm_state_reply(ewmh, xcb_ewmh_get_wm_state(ewmh, window),
                                  &states, nullptr)) {
    bool skip = false;
    for (unsigned i = 0; i < states.atoms_len; ++i) {
      if (states.atoms[i] == ewmh_._NET_WM_STATE_SKIP_TASKBAR) {
        skip = true;
        break;
      }
    }
    xcb_ewmh_get_atoms_reply_wipe(&states);
    if (skip) return false;
  }
  return true;
}

bool X11Platform::isMinimized(xcb_window_t window) const {
  auto* ewmh = const_cast<xcb_ewmh_connection_t*>(&ewmh_);
  xcb_ewmh_get_atoms_reply_t states{};
  if (!xcb_ewmh_get_wm_state_reply(ewmh, xcb_ewmh_get_wm_state(ewmh, window),
                                   &states, nullptr)) {
    return false;
  }
  bool hidden = false;
  for (unsigned i = 0; i < states.atoms_len; ++i) {
    if (states.atoms[i] == ewmh_._NET_WM_STATE_HIDDEN) {
      hidden = true;
      break;
    }
  }
  xcb_ewmh_get_atoms_reply_wipe(&states);
  return hidden;
}

std::vector<Toplevel> X11Platform::toplevels() {
  std::vector<Toplevel> out;
  if (!ewmhReady_) return out;

  xcb_window_t active = XCB_WINDOW_NONE;
  xcb_ewmh_get_active_window_reply(
      &ewmh_, xcb_ewmh_get_active_window(&ewmh_, screenNumber_), &active,
      nullptr);

  xcb_ewmh_get_windows_reply_t clients{};
  if (!xcb_ewmh_get_client_list_reply(
          &ewmh_, xcb_ewmh_get_client_list(&ewmh_, screenNumber_), &clients,
          nullptr)) {
    return out;
  }

  out.reserve(clients.windows_len);
  for (unsigned i = 0; i < clients.windows_len; ++i) {
    const xcb_window_t window = clients.windows[i];
    if (window == dockWindow_) continue;
    if (!shouldShowInTaskbar(window)) continue;

    Toplevel top;
    top.id = window;
    top.title = windowTitle(window);
    top.appId = windowAppId(window);
    top.minimized = isMinimized(window);
    top.active = (window == active);

    uint32_t desktop = 0;
    if (xcb_ewmh_get_wm_desktop_reply(
            &ewmh_, xcb_ewmh_get_wm_desktop(&ewmh_, window), &desktop,
            nullptr)) {
      top.workspace = static_cast<int>(desktop);
    }
    out.push_back(std::move(top));
  }
  xcb_ewmh_get_windows_reply_wipe(&clients);
  return out;
}

void X11Platform::activateToplevel(uint64_t id) {
  if (!ewmhReady_) return;
  const auto window = static_cast<xcb_window_t>(id);
  // Source indication 2 == "pager"; window managers trust pagers to raise
  // windows without focus-stealing prevention getting in the way.
  xcb_ewmh_request_change_active_window(&ewmh_, screenNumber_, window,
                                        XCB_EWMH_CLIENT_SOURCE_TYPE_OTHER,
                                        XCB_CURRENT_TIME, XCB_WINDOW_NONE);
  xcb_flush(conn_);
}

void X11Platform::minimizeToplevel(uint64_t id) {
  if (!ewmhReady_ || wmChangeState_ == XCB_ATOM_NONE) return;
  // ICCCM IconicState is what "minimise" means on X11; _NET_WM_STATE_HIDDEN is
  // a status the WM publishes, not a request clients may set.
  xcb_client_message_event_t event{};
  event.response_type = XCB_CLIENT_MESSAGE;
  event.format = 32;
  event.window = static_cast<xcb_window_t>(id);
  event.type = wmChangeState_;
  event.data.data32[0] = XCB_ICCCM_WM_STATE_ICONIC;
  xcb_send_event(conn_, 0, screen_->root,
                 XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                     XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
                 reinterpret_cast<const char*>(&event));
  xcb_flush(conn_);
}

bool X11Platform::translate(xcb_generic_event_t* raw, Event& out) {
  const uint8_t type = raw->response_type & 0x7F;
  switch (type) {
    case XCB_EXPOSE: {
      auto* e = reinterpret_cast<xcb_expose_event_t*>(raw);
      // Only the final expose of a burst needs to trigger a repaint; the
      // shell redraws whole surfaces rather than damage rectangles.
      if (e->count != 0) return false;
      out.type = Event::Type::Expose;
      return true;
    }
    case XCB_CONFIGURE_NOTIFY: {
      auto* e = reinterpret_cast<xcb_configure_notify_event_t*>(raw);
      out.type = Event::Type::Resize;
      out.size = {e->width, e->height};
      return true;
    }
    case XCB_MOTION_NOTIFY: {
      auto* e = reinterpret_cast<xcb_motion_notify_event_t*>(raw);
      out.type = Event::Type::MouseMove;
      out.position = {e->event_x, e->event_y};
      return true;
    }
    case XCB_BUTTON_PRESS: {
      auto* e = reinterpret_cast<xcb_button_press_event_t*>(raw);
      // 4/5 are wheel notches, not buttons; the taskbar does not use them yet.
      if (e->detail >= 4) return false;
      out.type = Event::Type::MouseDown;
      out.position = {e->event_x, e->event_y};
      out.button = toMouseButton(e->detail);
      return true;
    }
    case XCB_BUTTON_RELEASE: {
      auto* e = reinterpret_cast<xcb_button_release_event_t*>(raw);
      if (e->detail >= 4) return false;
      out.type = Event::Type::MouseUp;
      out.position = {e->event_x, e->event_y};
      out.button = toMouseButton(e->detail);
      return true;
    }
    case XCB_LEAVE_NOTIFY:
      out.type = Event::Type::MouseLeave;
      return true;
    case XCB_PROPERTY_NOTIFY: {
      auto* e = reinterpret_cast<xcb_property_notify_event_t*>(raw);
      if (e->atom == ewmh_._NET_CLIENT_LIST ||
          e->atom == ewmh_._NET_ACTIVE_WINDOW ||
          e->atom == ewmh_._NET_CURRENT_DESKTOP) {
        out.type = Event::Type::ToplevelsChanged;
        return true;
      }
      // A title or state change on a tracked window is equally a list change.
      if (e->atom == ewmh_._NET_WM_NAME || e->atom == ewmh_._NET_WM_STATE ||
          e->atom == XCB_ATOM_WM_NAME) {
        out.type = Event::Type::ToplevelsChanged;
        return true;
      }
      return false;
    }
    default:
      return false;
  }
}

bool X11Platform::pollEvent(Event& out) {
  if (!conn_) return false;
  if (xcb_connection_has_error(conn_)) {
    out.type = Event::Type::Quit;
    return true;
  }
  while (xcb_generic_event_t* raw = xcb_poll_for_event(conn_)) {
    const bool translated = translate(raw, out);
    free(raw);
    if (translated) return true;
  }
  return false;
}

bool X11Platform::waitEvent(Event& out) {
  if (!conn_) return false;
  for (;;) {
    xcb_generic_event_t* raw = xcb_wait_for_event(conn_);
    if (!raw) {
      out.type = Event::Type::Quit;
      return true;
    }
    const bool translated = translate(raw, out);
    free(raw);
    if (translated) return true;
  }
}

int X11Platform::eventFd() const {
  return conn_ ? xcb_get_file_descriptor(conn_) : -1;
}

}  // namespace finux::platform

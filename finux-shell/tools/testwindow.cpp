// A minimal EWMH-visible window, for exercising the taskbar by hand.
//
// The shell's interesting behaviour -- task buttons appearing, grouping by
// WM_CLASS, activation, strut reservation -- only exists in the presence of a
// window manager and real client windows. This produces those clients without
// dragging a terminal emulator or toolkit into the dependency list.
//
//   finux-testwindow "Untitled - Notepad" Notepad
//
// Runs until killed.

#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  const std::string title = argc > 1 ? argv[1] : "Test Window";
  const std::string appClass = argc > 2 ? argv[2] : "TestWindow";

  int screenNumber = 0;
  xcb_connection_t* conn = xcb_connect(nullptr, &screenNumber);
  if (!conn || xcb_connection_has_error(conn)) {
    std::fprintf(stderr, "testwindow: cannot connect to the display\n");
    return 1;
  }

  const xcb_setup_t* setup = xcb_get_setup(conn);
  xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
  for (int i = 0; i < screenNumber && it.rem; ++i) xcb_screen_next(&it);
  xcb_screen_t* screen = it.data;
  if (!screen) {
    std::fprintf(stderr, "testwindow: no screen\n");
    return 1;
  }

  const xcb_window_t window = xcb_generate_id(conn);
  const uint32_t values[] = {screen->white_pixel, XCB_EVENT_MASK_EXPOSURE};
  xcb_create_window(conn, XCB_COPY_FROM_PARENT, window, screen->root, 100, 100,
                    400, 300, 1, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    screen->root_visual,
                    XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);

  // _NET_WM_NAME is what the taskbar reads; WM_NAME is the legacy fallback.
  xcb_intern_atom_reply_t* netName = xcb_intern_atom_reply(
      conn, xcb_intern_atom(conn, 0, 12, "_NET_WM_NAME"), nullptr);
  xcb_intern_atom_reply_t* utf8 = xcb_intern_atom_reply(
      conn, xcb_intern_atom(conn, 0, 11, "UTF8_STRING"), nullptr);
  if (netName && utf8) {
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, window, netName->atom,
                        utf8->atom, 8, static_cast<uint32_t>(title.size()),
                        title.c_str());
  }
  free(netName);
  free(utf8);

  xcb_icccm_set_wm_name(conn, window, XCB_ATOM_STRING, 8,
                        static_cast<uint32_t>(title.size()), title.c_str());

  // WM_CLASS is two NUL-terminated strings: instance, then class.
  std::vector<char> wmClass;
  wmClass.insert(wmClass.end(), appClass.begin(), appClass.end());
  wmClass.push_back('\0');
  wmClass.insert(wmClass.end(), appClass.begin(), appClass.end());
  wmClass.push_back('\0');
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_CLASS,
                      XCB_ATOM_STRING, 8,
                      static_cast<uint32_t>(wmClass.size()), wmClass.data());

  xcb_map_window(conn, window);
  xcb_flush(conn);

  std::printf("testwindow: mapped 0x%x \"%s\" (%s)\n", window, title.c_str(),
              appClass.c_str());
  std::fflush(stdout);

  while (xcb_generic_event_t* event = xcb_wait_for_event(conn)) {
    free(event);
  }
  xcb_disconnect(conn);
  return 0;
}

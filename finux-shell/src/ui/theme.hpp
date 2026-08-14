#pragma once

#include "core/color.hpp"
#include "core/geometry.hpp"

namespace finux::ui {

// Every Windows 10 metric and colour the shell draws with, in one place.
//
// IMPORTANT: the values below are a starting point, not gospel. The only
// authority on "does this look exactly like Windows 10" is
// tests/pixdiff, which compares our rendering against captured reference
// screenshots pixel by pixel. When a pixdiff test disagrees with a number
// here, the number is wrong -- fix it here rather than special-casing the
// widget, so that every consumer of the metric moves together.
//
// All values are device pixels at 100% scaling (96 DPI). Scaled values are
// derived in Theme::scaled() rather than stored, because Windows rounds
// several of these to whole pixels per scale factor rather than multiplying
// cleanly.
struct Metrics {
  // --- Taskbar -------------------------------------------------------------
  int taskbarHeight = 40;       // 30 with "use small taskbar buttons"
  int taskbarHeightSmall = 30;
  int startButtonWidth = 48;
  int searchBoxWidth = 200;
  int taskViewButtonWidth = 40;
  int taskButtonWidth = 160;    // labelled button, before crowding kicks in
  int taskButtonWidthIconOnly = 40;
  int taskButtonMinWidth = 44;  // crowded floor before grouping takes over
  int taskbarIconSize = 24;
  int runningIndicatorHeight = 3;   // accent line under a running app
  int runningIndicatorInset = 10;   // horizontal inset of that line

  // --- Notification area ---------------------------------------------------
  int trayIconSize = 16;
  int trayIconSpacing = 24;
  int trayChevronWidth = 20;
  int clockWidth = 74;          // locale-dependent; measured, not assumed
  int showDesktopWidth = 8;     // the sliver at the far right edge

  // --- Common chrome -------------------------------------------------------
  int focusRingWidth = 1;
  int menuItemHeight = 22;
  int menuSeparatorHeight = 7;
  int menuPaddingVertical = 2;
  int menuBorderWidth = 1;

  // --- Type ----------------------------------------------------------------
  // Windows 10 shell UI is Segoe UI 9pt, which is 12px at 96 DPI.
  int uiFontPixelSize = 12;
};

struct Palette {
  // Windows' default accent ("Blue"). User accent colours replace this at
  // runtime; nothing may hardcode it.
  Color accent = Color::rgb(0x0078D7);
  Color accentLight = Color::rgb(0x429CE3);
  Color accentDark = Color::rgb(0x005A9E);

  // Taskbar with transparency effects disabled. With them enabled the bar is
  // this colour composited over a blurred backdrop, which is a later stage.
  Color taskbarBackground = Color::rgb(0x1F1F1F);
  Color taskbarText = Color::rgb(0xFFFFFF);

  // Hover/press layers are translucent white over whatever is beneath, which
  // is why they are stored with alpha rather than pre-flattened.
  Color taskButtonHover = Color::rgba(0xFFFFFF, 0x1A);
  Color taskButtonPressed = Color::rgba(0xFFFFFF, 0x0D);
  Color taskButtonActive = Color::rgba(0xFFFFFF, 0x26);

  Color startButtonHover = Color::rgba(0xFFFFFF, 0x1A);

  // --- Menus (the taskbar context menu in particular) ----------------------
  Color menuBackground = Color::rgb(0x2B2B2B);
  Color menuBorder = Color::rgb(0x454545);
  Color menuText = Color::rgb(0xFFFFFF);
  Color menuTextDisabled = Color::rgb(0x6D6D6D);
  Color menuHighlight = Color::rgba(0xFFFFFF, 0x1A);
  Color menuSeparator = Color::rgb(0x454545);
};

struct Theme {
  Metrics metrics;
  Palette palette;

  // Scale factor in percent (100, 125, 150, 175, 200), matching the values
  // Windows exposes. Kept as an integer because fractional DPI in a shell is
  // how you end up with 1px seams between the taskbar and the screen edge.
  int scalePercent = 100;

  static Theme win10() { return Theme{}; }

  int scaled(int value) const {
    return (value * scalePercent + 50) / 100;
  }

  int taskbarHeight(bool smallButtons = false) const {
    return scaled(smallButtons ? metrics.taskbarHeightSmall
                               : metrics.taskbarHeight);
  }
};

}  // namespace finux::ui

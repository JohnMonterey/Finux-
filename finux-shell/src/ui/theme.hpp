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
  int searchBoxWidth = 400;
  int searchBoxHeight = 30;
  int searchBoxMargin = 5;      // inset from the taskbar's top/bottom edge
  int cortanaButtonWidth = 48;
  int taskViewButtonWidth = 48;
  int taskButtonWidth = 160;    // labelled button, before crowding kicks in
  int taskButtonWidthIconOnly = 40;
  int taskButtonMinWidth = 44;  // crowded floor before grouping takes over
  int taskbarIconSize = 24;
  int runningIndicatorHeight = 3;   // accent line under a running app
  int runningIndicatorInset = 10;   // horizontal inset of that line

  // --- Notification area ---------------------------------------------------
  int trayIconSize = 16;
  int trayIconSpacing = 28;
  int trayChevronWidth = 24;
  int clockWidth = 74;          // locale-dependent; measured, not assumed
  int actionCenterWidth = 30;
  int showDesktopWidth = 8;     // the sliver at the far right edge

  // --- Start menu ----------------------------------------------------------
  //
  // The three-column layout: an icon rail, the alphabetical app list, and the
  // tile panel. Widths are derived from the tile grid rather than chosen
  // independently -- the panel exists to hold exactly six tile cells across,
  // so its width follows from the cell size and gutter.
  int startMenuRailWidth = 56;
  int startMenuAppListWidth = 320;
  int startMenuTilePadding = 24;
  int startMenuHeight = 762;

  // A "cell" is the small-tile footprint. Medium tiles are 2x2 cells, wide
  // tiles 4x2, large 4x4, each absorbing the gutters they span.
  int tileCell = 52;
  int tileGap = 8;
  int tileGridColumns = 6;

  int startAppRowHeight = 44;
  int startAppIconSize = 26;
  int startLetterHeaderHeight = 40;
  int startGroupHeaderHeight = 34;
  int startRailIconSize = 20;

  // --- Common chrome -------------------------------------------------------
  int focusRingWidth = 1;
  int menuItemHeight = 22;
  int menuSeparatorHeight = 7;
  int menuPaddingVertical = 2;
  int menuBorderWidth = 1;

  // --- Type ----------------------------------------------------------------
  // Windows 10 shell UI is Segoe UI 9pt, which is 12px at 96 DPI.
  int uiFontPixelSize = 12;
  int tileLabelFontPixelSize = 12;
  int groupHeaderFontPixelSize = 12;

  // Tile widths for each size, derived so a row of three mediums exactly
  // fills the six-column grid.
  int tileSpan(int cells) const {
    return cells * tileCell + (cells - 1) * tileGap;
  }
  int tileGridWidth() const { return tileSpan(tileGridColumns); }
  int startMenuTilePanelWidth() const {
    return tileGridWidth() + startMenuTilePadding * 2;
  }
  int startMenuWidth() const {
    return startMenuRailWidth + startMenuAppListWidth +
           startMenuTilePanelWidth();
  }
};

struct Palette {
  // Windows' default accent ("Blue"). User accent colours replace this at
  // runtime; nothing may hardcode it.
  Color accent = Color::rgb(0x0078D7);
  Color accentLight = Color::rgb(0x429CE3);
  Color accentDark = Color::rgb(0x005A9E);

  Color taskbarBackground = Color::rgb(0x1F1F1F);
  Color taskbarText = Color::rgb(0xFFFFFF);
  Color taskbarIcon = Color::rgb(0xFFFFFF);

  // The Start button takes the accent colour when "show accent on Start and
  // taskbar" is enabled, which is the default in the light theme.
  Color startButtonBackground = Color::rgba(0xFFFFFF, 0x00);
  Color startButtonGlyph = Color::rgb(0xFFFFFF);

  Color searchBoxBackground = Color::rgb(0x2B2B2B);
  Color searchBoxBorder = Color::rgb(0x4A4A4A);
  Color searchBoxText = Color::rgb(0xA0A0A0);

  // Hover/press layers are translucent over whatever is beneath, which is why
  // they are stored with alpha rather than pre-flattened.
  Color taskButtonHover = Color::rgba(0xFFFFFF, 0x1A);
  Color taskButtonPressed = Color::rgba(0xFFFFFF, 0x0D);
  Color taskButtonActive = Color::rgba(0xFFFFFF, 0x26);
  Color startButtonHover = Color::rgba(0xFFFFFF, 0x1A);

  // --- Start menu ----------------------------------------------------------
  Color startMenuBackground = Color::rgb(0x1F1F1F);
  Color startMenuTilePanel = Color::rgb(0x272727);
  Color startMenuRail = Color::rgb(0x1B1B1B);
  Color startMenuBorder = Color::rgb(0x3A3A3A);
  Color startMenuText = Color::rgb(0xFFFFFF);
  Color startMenuSubtleText = Color::rgb(0xA0A0A0);
  Color startMenuHover = Color::rgba(0xFFFFFF, 0x14);
  Color tileDefault = Color::rgb(0x2D2D2D);
  Color tileText = Color::rgb(0xFFFFFF);

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

  static Theme win10Dark() { return Theme{}; }

  // The default Windows 10 appearance: light shell surfaces, dark text, and
  // the accent colour reserved for the Start button and running indicators.
  static Theme win10Light() {
    Theme theme;
    Palette& p = theme.palette;

    p.taskbarBackground = Color::rgb(0xF3F3F3);
    p.taskbarText = Color::rgb(0x000000);
    p.taskbarIcon = Color::rgb(0x000000);

    p.startButtonBackground = p.accent;
    p.startButtonGlyph = Color::rgb(0xFFFFFF);
    p.startButtonHover = Color::rgba(0xFFFFFF, 0x33);

    p.searchBoxBackground = Color::rgb(0xFFFFFF);
    p.searchBoxBorder = Color::rgb(0xCCCCCC);
    p.searchBoxText = Color::rgb(0x5A5A5A);

    // Dark ink over a light bar, so the hover layers invert.
    p.taskButtonHover = Color::rgba(0x000000, 0x14);
    p.taskButtonPressed = Color::rgba(0x000000, 0x0A);
    p.taskButtonActive = Color::rgba(0x000000, 0x0F);

    p.startMenuBackground = Color::rgb(0xF2F2F2);
    p.startMenuTilePanel = Color::rgb(0xEAEDF0);
    p.startMenuRail = Color::rgb(0xEBEBEB);
    p.startMenuBorder = Color::rgb(0xD6D6D6);
    p.startMenuText = Color::rgb(0x000000);
    p.startMenuSubtleText = Color::rgb(0x5A5A5A);
    p.startMenuHover = Color::rgba(0x000000, 0x0F);
    p.tileDefault = Color::rgb(0xDDE3E9);
    p.tileText = Color::rgb(0x000000);

    p.menuBackground = Color::rgb(0xF2F2F2);
    p.menuBorder = Color::rgb(0xD6D6D6);
    p.menuText = Color::rgb(0x000000);
    p.menuTextDisabled = Color::rgb(0x9A9A9A);
    p.menuHighlight = Color::rgba(0x000000, 0x0F);
    p.menuSeparator = Color::rgb(0xD6D6D6);
    return theme;
  }

  // Light is what Windows 10 ships with, so it is what "default" means here.
  static Theme win10() { return win10Light(); }

  int scaled(int value) const { return (value * scalePercent + 50) / 100; }

  int taskbarHeight(bool smallButtons = false) const {
    return scaled(smallButtons ? metrics.taskbarHeightSmall
                               : metrics.taskbarHeight);
  }
};

}  // namespace finux::ui

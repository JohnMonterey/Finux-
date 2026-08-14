#include "shell/taskbar.hpp"

#include <algorithm>

#include "ui/glyphs.hpp"

namespace finux::shell {

// The glyph helpers live in ui; alias them so call sites read as
// glyphs::search(...) rather than ui::glyphs::search(...).
namespace glyphs = ui::glyphs;

using finux::text::HAlign;
using finux::text::VAlign;

// ---------------------------------------------------------------------------
// TaskbarButton
// ---------------------------------------------------------------------------

void TaskbarButton::onMouseUp(const ui::MouseEvent& event) {
  // Fire only if the release lands inside the button, matching the standard
  // click contract: press, drag out, release must not activate.
  if (onClick_ && localBounds().contains(event.position)) onClick_(event.button);
}

void TaskbarButton::paintBackground(PaintContext& ctx, Color hover,
                                    Color pressedColor) {
  if (pressed()) {
    ctx.fill(localBounds(), pressedColor);
  } else if (hovered()) {
    ctx.fill(localBounds(), hover);
  }
}

// ---------------------------------------------------------------------------
// StartButton
// ---------------------------------------------------------------------------

void StartButton::onPaint(PaintContext& ctx) {
  const ui::Palette& palette = ctx.theme.palette;
  const Rect box = localBounds();

  // In the light theme the Start button carries the accent colour, which is
  // why the glyph colour is tracked separately from the taskbar's ink.
  ctx.fill(box, palette.startButtonBackground);
  paintBackground(ctx, palette.startButtonHover, palette.startButtonHover);

  const int size = ctx.theme.scaled(20);
  const Rect glyph{box.x + (box.w - size) / 2, box.y + (box.h - size) / 2, size,
                   size};
  glyphs::windowsLogo(ctx.canvas, ctx.toSurface(glyph), palette.startButtonGlyph);
}

// ---------------------------------------------------------------------------
// SearchBox
// ---------------------------------------------------------------------------

void SearchBox::onPaint(PaintContext& ctx) {
  const ui::Palette& palette = ctx.theme.palette;
  const Rect box = localBounds();

  ctx.fill(box, palette.searchBoxBackground);
  ctx.canvas.strokeRectInside(ctx.toSurface(box), palette.searchBoxBorder, 1);

  const int icon = ctx.theme.scaled(14);
  const int pad = ctx.theme.scaled(10);
  const Rect iconRect{box.x + pad, box.y + (box.h - icon) / 2, icon, icon};
  glyphs::search(ctx.canvas, ctx.toSurface(iconRect), palette.searchBoxText);

  const int textX = iconRect.right() + ctx.theme.scaled(10);
  ctx.label({textX, box.y, box.right() - textX - pad, box.h}, placeholder_,
            palette.searchBoxText, HAlign::Left, VAlign::Middle);
}

// ---------------------------------------------------------------------------
// CortanaButton / TaskViewButton
// ---------------------------------------------------------------------------

void CortanaButton::onPaint(PaintContext& ctx) {
  const ui::Palette& palette = ctx.theme.palette;
  paintBackground(ctx, palette.taskButtonHover, palette.taskButtonPressed);
  const Rect box = localBounds();
  const int size = ctx.theme.scaled(18);
  glyphs::cortanaRing(
      ctx.canvas,
      ctx.toSurface({box.x + (box.w - size) / 2, box.y + (box.h - size) / 2,
                     size, size}),
      palette.taskbarIcon);
}

void TaskViewButton::onPaint(PaintContext& ctx) {
  const ui::Palette& palette = ctx.theme.palette;
  paintBackground(ctx, palette.taskButtonHover, palette.taskButtonPressed);
  const Rect box = localBounds();
  const int size = ctx.theme.scaled(20);
  glyphs::taskView(
      ctx.canvas,
      ctx.toSurface({box.x + (box.w - size) / 2, box.y + (box.h - size) / 2,
                     size, size}),
      palette.taskbarIcon);
}

// ---------------------------------------------------------------------------
// PinnedButton
// ---------------------------------------------------------------------------

void PinnedButton::setApp(std::string name, Color color) {
  name_ = std::move(name);
  color_ = color;
  invalidate();
}

void PinnedButton::setRunning(bool running) {
  if (running_ == running) return;
  running_ = running;
  invalidate();
}

void PinnedButton::onPaint(PaintContext& ctx) {
  const ui::Metrics& metrics = ctx.theme.metrics;
  const ui::Palette& palette = ctx.theme.palette;
  const Rect box = localBounds();

  paintBackground(ctx, palette.taskButtonHover, palette.taskButtonPressed);

  const int iconSize = ctx.theme.scaled(metrics.taskbarIconSize);
  const Rect icon{box.x + (box.w - iconSize) / 2, box.y + (box.h - iconSize) / 2,
                  iconSize, iconSize};
  glyphs::appPlaceholder(ctx.canvas, ctx.toSurface(icon), color_);
  if (!name_.empty()) {
    ctx.label(icon, name_.substr(0, 1), Color::rgb(0xFFFFFF), HAlign::Center,
              VAlign::Middle);
  }

  if (running_) {
    const int height = ctx.theme.scaled(metrics.runningIndicatorHeight);
    const int inset = ctx.theme.scaled(metrics.runningIndicatorInset);
    ctx.fill({box.x + inset, box.bottom() - height, box.w - inset * 2, height},
             palette.accent);
  }
}

// ---------------------------------------------------------------------------
// TaskButton
// ---------------------------------------------------------------------------

void TaskButton::setToplevel(const platform::Toplevel& toplevel) {
  toplevel_ = toplevel;
  invalidate();
}

void TaskButton::onPaint(PaintContext& ctx) {
  const ui::Metrics& metrics = ctx.theme.metrics;
  const ui::Palette& palette = ctx.theme.palette;
  const Rect box = localBounds();

  // An active window keeps its lighter fill even when the pointer is
  // elsewhere; hover layers over the top of it.
  if (toplevel_.active) ctx.fill(box, palette.taskButtonActive);
  paintBackground(ctx, palette.taskButtonHover, palette.taskButtonPressed);

  // Running indicator: an accent line along the bottom edge. Minimised windows
  // still show it -- Windows distinguishes minimised by fill, not by the line.
  const int indicatorHeight = ctx.theme.scaled(metrics.runningIndicatorHeight);
  const int inset = ctx.theme.scaled(metrics.runningIndicatorInset);
  ctx.fill({box.x + inset, box.bottom() - indicatorHeight, box.w - inset * 2,
            indicatorHeight},
           palette.accent);

  // Placeholder application icon.
  //
  // Real icons come from _NET_WM_ICON or the XDG icon theme via the .desktop
  // entry matching WM_CLASS; neither is wired up yet.
  // A crowded bar centres the icon and drops the label entirely; a roomy one
  // left-aligns it and writes the window title alongside.
  const int iconSize = ctx.theme.scaled(metrics.taskbarIconSize);
  const bool iconOnly =
      box.w < ctx.theme.scaled(metrics.taskButtonWidth) - ctx.theme.scaled(8);
  const int iconX = iconOnly ? box.x + (box.w - iconSize) / 2
                             : box.x + ctx.theme.scaled(8);
  const Rect iconRect{iconX, box.y + (box.h - iconSize) / 2, iconSize, iconSize};
  glyphs::appPlaceholder(ctx.canvas, ctx.toSurface(iconRect), palette.accentDark);
  if (!toplevel_.appId.empty()) {
    ctx.label(iconRect, toplevel_.appId.substr(0, 1), Color::rgb(0xFFFFFF),
              HAlign::Center, VAlign::Middle);
  }

  if (iconOnly) return;

  const int labelX = iconRect.right() + ctx.theme.scaled(8);
  const int labelWidth = box.right() - ctx.theme.scaled(8) - labelX;
  if (labelWidth > ctx.theme.scaled(16)) {
    ctx.label({labelX, box.y, labelWidth, box.h - indicatorHeight},
              toplevel_.title, palette.taskbarText, HAlign::Left,
              VAlign::Middle);
  }
}

// ---------------------------------------------------------------------------
// SystemTray
// ---------------------------------------------------------------------------

int SystemTray::preferredWidth(PaintContext& ctx) const {
  const ui::Metrics& m = ctx.theme.metrics;
  // Chevron plus four status icons.
  return ctx.theme.scaled(m.trayChevronWidth) + 4 * ctx.theme.scaled(m.trayIconSpacing);
}

void SystemTray::onPaint(PaintContext& ctx) {
  const ui::Metrics& m = ctx.theme.metrics;
  const Color ink = ctx.theme.palette.taskbarIcon;
  const Rect box = localBounds();

  const int icon = ctx.theme.scaled(m.trayIconSize);
  const int spacing = ctx.theme.scaled(m.trayIconSpacing);
  const int y = box.y + (box.h - icon) / 2;

  int x = box.x;
  const int chevron = ctx.theme.scaled(m.trayChevronWidth);
  glyphs::chevronUp(ctx.canvas,
                    ctx.toSurface({x + (chevron - icon) / 2, y, icon, icon}),
                    ink);
  x += chevron;

  using Painter = void (*)(gfx::Canvas&, Rect, Color);
  const Painter icons[] = {glyphs::cloud, glyphs::picture, glyphs::network,
                           glyphs::volume};
  for (Painter paint : icons) {
    paint(ctx.canvas, ctx.toSurface({x + (spacing - icon) / 2, y, icon, icon}),
          ink);
    x += spacing;
  }
}

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

void Clock::setTime(std::string time, std::string date) {
  if (time == time_ && date == date_) return;
  time_ = std::move(time);
  date_ = std::move(date);
  invalidate();
}

int Clock::preferredWidth(PaintContext& ctx) const {
  const int widest = std::max(ctx.text.measure(ctx.uiFont, time_),
                              ctx.text.measure(ctx.uiFont, date_));
  return widest + ctx.theme.scaled(24);
}

void Clock::onPaint(PaintContext& ctx) {
  const ui::Palette& palette = ctx.theme.palette;
  paintBackground(ctx, palette.taskButtonHover, palette.taskButtonPressed);

  const Rect box = localBounds();
  const int half = box.h / 2;
  const int padRight = ctx.theme.scaled(12);
  const Rect textArea = box.inset(0, 0, padRight, 0);

  ctx.label({textArea.x, textArea.y, textArea.w, half}, time_,
            palette.taskbarText, HAlign::Right, VAlign::Middle);
  ctx.label({textArea.x, textArea.y + half, textArea.w, box.h - half}, date_,
            palette.taskbarText, HAlign::Right, VAlign::Middle);
}

// ---------------------------------------------------------------------------
// ShowDesktopButton
// ---------------------------------------------------------------------------

void ShowDesktopButton::onPaint(PaintContext& ctx) {
  const Rect box = localBounds();
  paintBackground(ctx, ctx.theme.palette.taskButtonHover,
                  ctx.theme.palette.taskButtonPressed);
  // A single hairline on the left edge is the only thing that marks it.
  ctx.canvas.vLine(ctx.toSurface(box).x, ctx.toSurface(box).y + 2, box.h - 4,
                   ctx.theme.palette.startMenuBorder);
}

// ---------------------------------------------------------------------------
// Taskbar
// ---------------------------------------------------------------------------

std::vector<Taskbar::PinnedApp> Taskbar::defaultPinnedApps() {
  return {
      {"File Explorer", Color::rgb(0xFFB900)},
      {"Microsoft Edge", Color::rgb(0x0078D7)},
  };
}

Taskbar::Taskbar(const ui::Theme& theme) : theme_(theme) {
  pinnedApps_ = defaultPinnedApps();
  rebuild();
}

void Taskbar::setPinnedApps(std::vector<PinnedApp> apps) {
  pinnedApps_ = std::move(apps);
  rebuild();
}

void Taskbar::setCombineButtons(bool combine) {
  if (combineButtons_ == combine) return;
  combineButtons_ = combine;
  needsLayout_ = true;
  invalidate();
}

void Taskbar::setClock(std::string time, std::string date) {
  if (clock_) clock_->setTime(std::move(time), std::move(date));
}

void Taskbar::setToplevels(const std::vector<platform::Toplevel>& toplevels) {
  toplevels_ = toplevels;
  rebuild();
}

// Rebuild wholesale. The list is small (tens of entries at most) and the
// alternative -- diffing against the previous set -- has repeatedly proven to
// be where panel implementations grow stale-button bugs.
void Taskbar::rebuild() {
  taskButtons_.clear();
  pinnedButtons_.clear();
  clearChildren();

  auto start = std::make_unique<StartButton>();
  start->setOnClick([this](ui::MouseButton button) {
    if (onStartClicked) onStartClicked(button);
  });
  start_ = static_cast<StartButton*>(addChild(std::move(start)));

  search_ = static_cast<SearchBox*>(addChild(std::make_unique<SearchBox>()));
  cortana_ =
      static_cast<CortanaButton*>(addChild(std::make_unique<CortanaButton>()));
  taskView_ =
      static_cast<TaskViewButton*>(addChild(std::make_unique<TaskViewButton>()));

  for (const PinnedApp& app : pinnedApps_) {
    auto button = std::make_unique<PinnedButton>();
    button->setApp(app.name, app.color);
    // A pinned app that is also running shows the indicator rather than
    // getting a second, separate button.
    const bool running = std::any_of(
        toplevels_.begin(), toplevels_.end(),
        [&app](const platform::Toplevel& t) { return t.appId == app.name; });
    button->setRunning(running);
    pinnedButtons_.push_back(
        static_cast<PinnedButton*>(addChild(std::move(button))));
  }

  for (const platform::Toplevel& toplevel : toplevels_) {
    const bool pinned =
        std::any_of(pinnedApps_.begin(), pinnedApps_.end(),
                    [&toplevel](const PinnedApp& a) {
                      return a.name == toplevel.appId;
                    });
    if (pinned) continue;

    auto button = std::make_unique<TaskButton>();
    button->setToplevel(toplevel);
    const uint64_t id = toplevel.id;
    button->setOnClick([this, id](ui::MouseButton mouseButton) {
      if (onTaskActivated) onTaskActivated(id, mouseButton);
    });
    taskButtons_.push_back(static_cast<TaskButton*>(addChild(std::move(button))));
  }

  tray_ = static_cast<SystemTray*>(addChild(std::make_unique<SystemTray>()));
  clock_ = static_cast<Clock*>(addChild(std::make_unique<Clock>()));
  showDesktop_ = static_cast<ShowDesktopButton*>(
      addChild(std::make_unique<ShowDesktopButton>()));

  needsLayout_ = true;
  invalidate();
}

void Taskbar::onLayout() { needsLayout_ = true; }

void Taskbar::layoutIfNeeded(PaintContext& ctx) {
  if (!needsLayout_) return;
  needsLayout_ = false;

  const ui::Metrics& m = theme_.metrics;
  const Rect box = localBounds();
  if (box.empty()) return;

  // --- left cluster --------------------------------------------------------
  int x = 0;
  const int startWidth = theme_.scaled(m.startButtonWidth);
  start_->setBounds({x, 0, startWidth, box.h});
  x += startWidth;

  const int searchMargin = theme_.scaled(m.searchBoxMargin);
  const int searchHeight = theme_.scaled(m.searchBoxHeight);
  const int searchWidth = theme_.scaled(m.searchBoxWidth);
  search_->setBounds({x + searchMargin, (box.h - searchHeight) / 2, searchWidth,
                      searchHeight});
  x += searchWidth + searchMargin * 2;

  const int cortanaWidth = theme_.scaled(m.cortanaButtonWidth);
  cortana_->setBounds({x, 0, cortanaWidth, box.h});
  x += cortanaWidth;

  const int taskViewWidth = theme_.scaled(m.taskViewButtonWidth);
  taskView_->setBounds({x, 0, taskViewWidth, box.h});
  x += taskViewWidth + theme_.scaled(8);

  // --- right cluster, laid out from the screen edge inward -----------------
  int right = box.w;
  const int showDesktopWidth = theme_.scaled(m.showDesktopWidth);
  showDesktop_->setBounds({right - showDesktopWidth, 0, showDesktopWidth,
                           box.h});
  right -= showDesktopWidth;

  const int actionWidth = theme_.scaled(m.actionCenterWidth);
  right -= actionWidth;

  const int clockWidth = clock_->preferredWidth(ctx);
  clock_->setBounds({right - clockWidth, 0, clockWidth, box.h});
  right -= clockWidth;

  const int trayWidth = tray_->preferredWidth(ctx);
  tray_->setBounds({right - trayWidth, 0, trayWidth, box.h});
  right -= trayWidth;

  // --- pinned and task buttons fill what is left ---------------------------
  const int pinnedWidth = theme_.scaled(m.taskButtonWidthIconOnly);
  for (PinnedButton* button : pinnedButtons_) {
    const int width = std::min(pinnedWidth, std::max(0, right - x));
    button->setBounds({x, 0, width, box.h});
    button->setVisible(width > 0);
    x += width;
  }

  const int available = right - x;
  const int count = static_cast<int>(taskButtons_.size());
  if (count == 0 || available <= 0) return;

  // Task buttons take their preferred width until they no longer fit, then
  // share the remaining space equally, down to a floor. Windows switches to
  // icon-only buttons at that floor rather than shrinking further.
  const int preferred = theme_.scaled(combineButtons_ ? m.taskButtonWidthIconOnly
                                                      : m.taskButtonWidth);
  const int minimum =
      theme_.scaled(combineButtons_ ? m.taskButtonWidthIconOnly
                                    : m.taskButtonMinWidth);
  int width = std::min(preferred, available / count);
  width = std::max(width, minimum);

  for (TaskButton* button : taskButtons_) {
    const int clamped = std::min(width, std::max(0, right - x));
    button->setBounds({x, 0, clamped, box.h});
    button->setVisible(clamped > 0);
    x += clamped;
  }
}

void Taskbar::onPaint(PaintContext& ctx) {
  // Layout runs here because it needs measured text, and text measurement
  // needs the font that only the paint context carries.
  layoutIfNeeded(ctx);
  ctx.fill(localBounds(), theme_.palette.taskbarBackground);

  // Action Center lives at the right edge, drawn by the bar rather than as a
  // child because it has no hover state of its own yet.
  const ui::Metrics& m = theme_.metrics;
  const int icon = ctx.theme.scaled(m.trayIconSize);
  const int actionWidth = ctx.theme.scaled(m.actionCenterWidth);
  const int showDesktopWidth = ctx.theme.scaled(m.showDesktopWidth);
  const Rect box = localBounds();
  glyphs::actionCenter(
      ctx.canvas,
      ctx.toSurface({box.right() - showDesktopWidth - actionWidth +
                         (actionWidth - icon) / 2,
                     box.y + (box.h - icon) / 2, icon, icon}),
      theme_.palette.taskbarIcon);
}

}  // namespace finux::shell

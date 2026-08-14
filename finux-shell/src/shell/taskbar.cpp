#include "shell/taskbar.hpp"

#include <algorithm>

namespace finux::shell {

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
  paintBackground(ctx, palette.startButtonHover, palette.startButtonHover);

  // Placeholder four-pane glyph.
  //
  // The real Windows logo is a Microsoft trademark and is not shipped here.
  // The icon set is expected to be supplied at runtime (see docs/ASSETS.md);
  // this shape exists so the button has correct geometry and hit area while
  // the rendering stack is being validated.
  const Rect box = localBounds();
  const int pane = 8;
  const int gap = 2;
  const int total = pane * 2 + gap;
  const int x = box.x + (box.w - total) / 2;
  const int y = box.y + (box.h - total) / 2;
  const Color glyph = palette.taskbarText;

  ctx.fill({x, y, pane, pane}, glyph);
  ctx.fill({x + pane + gap, y, pane, pane}, glyph);
  ctx.fill({x, y + pane + gap, pane, pane}, glyph);
  ctx.fill({x + pane + gap, y + pane + gap, pane, pane}, glyph);
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
  // entry matching WM_CLASS; neither is wired up yet. Drawing a tinted tile
  // with the app's initial keeps the geometry honest in the meantime.
  const int iconSize = ctx.theme.scaled(metrics.taskbarIconSize);
  const int iconX = box.x + ctx.theme.scaled(8);
  const int iconY = box.y + (box.h - iconSize) / 2;
  const Rect iconRect{iconX, iconY, iconSize, iconSize};
  ctx.fill(iconRect, palette.accentDark);
  if (!toplevel_.appId.empty()) {
    ctx.label(iconRect, toplevel_.appId.substr(0, 1), palette.taskbarText,
              HAlign::Center, VAlign::Middle);
  }

  // The label is omitted on narrow (icon-only) buttons, as Windows does when
  // the bar is crowded or labels are turned off.
  const int labelX = iconRect.right() + ctx.theme.scaled(8);
  const int labelWidth = box.right() - ctx.theme.scaled(8) - labelX;
  if (labelWidth > ctx.theme.scaled(16)) {
    ctx.label({labelX, box.y, labelWidth, box.h - indicatorHeight},
              toplevel_.title, palette.taskbarText, HAlign::Left,
              VAlign::Middle);
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
  return widest + ctx.theme.scaled(20);
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
// Taskbar
// ---------------------------------------------------------------------------

Taskbar::Taskbar(const ui::Theme& theme) : theme_(theme) {
  auto start = std::make_unique<StartButton>();
  start->setOnClick([this](ui::MouseButton button) {
    if (onStartClicked) onStartClicked(button);
  });
  start_ = static_cast<StartButton*>(addChild(std::move(start)));

  clock_ = static_cast<Clock*>(addChild(std::make_unique<Clock>()));
}

void Taskbar::setClock(std::string time, std::string date) {
  if (clock_) clock_->setTime(std::move(time), std::move(date));
}

void Taskbar::setToplevels(const std::vector<platform::Toplevel>& toplevels) {
  // Rebuild wholesale. The list is small (tens of entries at most) and the
  // alternative -- diffing against the previous set -- has repeatedly proven
  // to be where panel implementations grow stale-button bugs.
  //
  // Children are owned by the widget tree, so this drops every child; the
  // start button and clock are recreated below along with the task buttons.
  taskButtons_.clear();
  clearChildren();
  start_ = nullptr;
  clock_ = nullptr;

  auto start = std::make_unique<StartButton>();
  start->setOnClick([this](ui::MouseButton button) {
    if (onStartClicked) onStartClicked(button);
  });
  start_ = static_cast<StartButton*>(addChild(std::move(start)));

  for (const platform::Toplevel& toplevel : toplevels) {
    auto button = std::make_unique<TaskButton>();
    button->setToplevel(toplevel);
    const uint64_t id = toplevel.id;
    button->setOnClick([this, id](ui::MouseButton mouseButton) {
      if (onTaskActivated) onTaskActivated(id, mouseButton);
    });
    taskButtons_.push_back(static_cast<TaskButton*>(addChild(std::move(button))));
  }

  clock_ = static_cast<Clock*>(addChild(std::make_unique<Clock>()));
  needsLayout_ = true;
  invalidate();
}

void Taskbar::onLayout() { needsLayout_ = true; }

void Taskbar::layoutIfNeeded(PaintContext& ctx) {
  if (!needsLayout_) return;
  needsLayout_ = false;

  const ui::Metrics& metrics = theme_.metrics;
  const Rect box = localBounds();
  if (box.empty()) return;

  const int startWidth = theme_.scaled(metrics.startButtonWidth);
  start_->setBounds({0, 0, startWidth, box.h});

  const int clockWidth = clock_->preferredWidth(ctx);
  const int showDesktop = theme_.scaled(metrics.showDesktopWidth);
  clock_->setBounds({box.w - showDesktop - clockWidth, 0, clockWidth, box.h});

  // Task buttons take their preferred width until they no longer fit, then
  // share the remaining space equally, down to a floor. Windows switches to
  // icon-only buttons at that floor rather than shrinking further.
  const int available = clock_->bounds().x - startWidth;
  const int count = static_cast<int>(taskButtons_.size());
  if (count == 0 || available <= 0) return;

  const int preferred = theme_.scaled(metrics.taskButtonWidth);
  const int minimum = theme_.scaled(metrics.taskButtonMinWidth);
  int width = std::min(preferred, available / count);
  width = std::max(width, minimum);

  int x = startWidth;
  for (TaskButton* button : taskButtons_) {
    const int clamped = std::min(width, std::max(0, startWidth + available - x));
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
}

}  // namespace finux::shell

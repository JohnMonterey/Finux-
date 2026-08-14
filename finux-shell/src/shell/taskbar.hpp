#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "platform/platform.hpp"
#include "ui/widget.hpp"

namespace finux::shell {

using ui::PaintContext;
using ui::Widget;

// Shared hover/press behaviour for the flat, borderless buttons the Windows 10
// taskbar is made of.
class TaskbarButton : public Widget {
 public:
  using Callback = std::function<void(ui::MouseButton)>;
  void setOnClick(Callback callback) { onClick_ = std::move(callback); }

 protected:
  bool interactive() const override { return true; }
  void onMouseUp(const ui::MouseEvent& event) override;
  // Draws the hover/press layer. Subclasses call this first, then their glyph.
  void paintBackground(PaintContext& ctx, Color hover, Color pressed);

 private:
  Callback onClick_;
};

class StartButton final : public TaskbarButton {
 protected:
  void onPaint(PaintContext& ctx) override;
};

// "Type here to search". A text field rather than a button, but it behaves as
// one until the search flyout exists.
class SearchBox final : public TaskbarButton {
 public:
  void setPlaceholder(std::string text) { placeholder_ = std::move(text); }

 protected:
  void onPaint(PaintContext& ctx) override;

 private:
  std::string placeholder_ = "Type here to search";
};

class CortanaButton final : public TaskbarButton {
 protected:
  void onPaint(PaintContext& ctx) override;
};

class TaskViewButton final : public TaskbarButton {
 protected:
  void onPaint(PaintContext& ctx) override;
};

// A pinned application, shown whether or not it is running.
class PinnedButton final : public TaskbarButton {
 public:
  void setApp(std::string name, Color color);
  void setRunning(bool running);

 protected:
  void onPaint(PaintContext& ctx) override;

 private:
  std::string name_;
  Color color_ = Color::rgb(0x0078D7);
  bool running_ = false;
};

// One running application.
class TaskButton final : public TaskbarButton {
 public:
  void setToplevel(const platform::Toplevel& toplevel);
  const platform::Toplevel& toplevel() const { return toplevel_; }

 protected:
  void onPaint(PaintContext& ctx) override;

 private:
  platform::Toplevel toplevel_;
};

// The notification area: overflow chevron, status icons, then the clock.
class SystemTray final : public TaskbarButton {
 public:
  int preferredWidth(PaintContext& ctx) const;

 protected:
  void onPaint(PaintContext& ctx) override;
};

// Time over date, right-aligned, as the 40px taskbar shows it.
class Clock final : public TaskbarButton {
 public:
  // Injected rather than read from the system clock inside onPaint, so that
  // pixel-diff tests can render a fixed time and stay reproducible.
  void setTime(std::string time, std::string date);
  int preferredWidth(PaintContext& ctx) const;

 protected:
  void onPaint(PaintContext& ctx) override;

 private:
  std::string time_ = "00:00";
  std::string date_ = "1/1/2020";
};

// The narrow strip at the far right that minimises everything on click.
class ShowDesktopButton final : public TaskbarButton {
 protected:
  void onPaint(PaintContext& ctx) override;
};

// The bar itself: owns layout and rebuilds its task buttons from the window
// list the platform reports.
class Taskbar final : public Widget {
 public:
  explicit Taskbar(const ui::Theme& theme);

  void setToplevels(const std::vector<platform::Toplevel>& toplevels);
  void setClock(std::string time, std::string date);

  struct PinnedApp {
    std::string name;
    Color color;
  };
  void setPinnedApps(std::vector<PinnedApp> apps);
  static std::vector<PinnedApp> defaultPinnedApps();

  // "Combine taskbar buttons: always" -- icon-only buttons with no label,
  // which is the Windows default and what a stock desktop shows.
  void setCombineButtons(bool combine);
  bool combineButtons() const { return combineButtons_; }

  // Invoked when a task button is clicked, with the window id.
  std::function<void(uint64_t, ui::MouseButton)> onTaskActivated;
  std::function<void(ui::MouseButton)> onStartClicked;

  // Layout needs measured text, which needs a paint context; the bar therefore
  // relays out lazily at paint time rather than in setBounds().
  void layoutIfNeeded(PaintContext& ctx);

  // Structure accessors.
  //
  // Tests assert against the bar's actual layout rather than recomputing it
  // from metrics: a test that re-derives positions is really a second
  // implementation of layout, and it silently agrees with itself when the real
  // one changes. These are empty/null only before the first rebuild.
  const std::vector<TaskButton*>& taskButtons() const { return taskButtons_; }
  const std::vector<PinnedButton*>& pinnedButtons() const {
    return pinnedButtons_;
  }
  const StartButton* startButton() const { return start_; }
  const SearchBox* searchBox() const { return search_; }
  const SystemTray* systemTray() const { return tray_; }
  const Clock* clockWidget() const { return clock_; }

 protected:
  void onPaint(PaintContext& ctx) override;
  void onLayout() override;
  bool interactive() const override { return true; }

 private:
  void rebuild();

  const ui::Theme& theme_;
  StartButton* start_ = nullptr;
  SearchBox* search_ = nullptr;
  CortanaButton* cortana_ = nullptr;
  TaskViewButton* taskView_ = nullptr;
  SystemTray* tray_ = nullptr;
  Clock* clock_ = nullptr;
  ShowDesktopButton* showDesktop_ = nullptr;
  std::vector<PinnedButton*> pinnedButtons_;
  std::vector<TaskButton*> taskButtons_;
  std::vector<PinnedApp> pinnedApps_;
  std::vector<platform::Toplevel> toplevels_;
  bool combineButtons_ = true;
  bool needsLayout_ = true;
};

}  // namespace finux::shell

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

// The bar itself: owns layout and rebuilds its task buttons from the window
// list the platform reports.
class Taskbar final : public Widget {
 public:
  explicit Taskbar(const ui::Theme& theme);

  void setToplevels(const std::vector<platform::Toplevel>& toplevels);
  void setClock(std::string time, std::string date);

  // Invoked when a task button is clicked, with the window id.
  std::function<void(uint64_t, ui::MouseButton)> onTaskActivated;
  std::function<void(ui::MouseButton)> onStartClicked;

  // Layout needs measured text, which needs a paint context; the bar therefore
  // relays out lazily at paint time rather than in setBounds().
  void layoutIfNeeded(PaintContext& ctx);

 protected:
  void onPaint(PaintContext& ctx) override;
  void onLayout() override;
  bool interactive() const override { return true; }

 private:
  const ui::Theme& theme_;
  StartButton* start_ = nullptr;
  Clock* clock_ = nullptr;
  std::vector<TaskButton*> taskButtons_;
  bool needsLayout_ = true;
};

}  // namespace finux::shell

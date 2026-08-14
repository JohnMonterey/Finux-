#include "shell/startmenu.hpp"

#include <algorithm>
#include <cctype>

#include "ui/acrylic.hpp"
#include "ui/glyphs.hpp"

namespace finux::shell {

// The glyph helpers live in ui; alias them so call sites read as
// glyphs::search(...) rather than ui::glyphs::search(...).
namespace glyphs = ui::glyphs;

using finux::text::HAlign;
using finux::text::VAlign;

Size tileCells(TileSize size) {
  switch (size) {
    case TileSize::Small: return {1, 1};
    case TileSize::Medium: return {2, 2};
    case TileSize::Wide: return {4, 2};
    case TileSize::Large: return {4, 4};
  }
  return {2, 2};
}

namespace {

// First character of an app name, uppercased, for the letter headers.
char initialOf(const std::string& name) {
  if (name.empty()) return '#';
  const auto c = static_cast<unsigned char>(name[0]);
  return static_cast<char>(std::toupper(c));
}

// A first-fit packer over a fixed-width grid of cells.
//
// Windows lets users drag tiles anywhere and stores explicit positions; until
// there is a tile-layout store to read, packing in declaration order
// reproduces the default arrangement, which is what a fresh install shows.
class TileGrid {
 public:
  TileGrid(int columns, int cell, int gap)
      : columns_(columns), cell_(cell), gap_(gap) {}

  Rect place(TileSize size) {
    const Size cells = tileCells(size);
    const int width = std::min(cells.w, columns_);
    for (int row = 0;; ++row) {
      ensureRows(row + cells.h);
      for (int column = 0; column + width <= columns_; ++column) {
        if (!fits(row, column, width, cells.h)) continue;
        occupy(row, column, width, cells.h);
        return {column * (cell_ + gap_), row * (cell_ + gap_),
                width * cell_ + (width - 1) * gap_,
                cells.h * cell_ + (cells.h - 1) * gap_};
      }
    }
  }

  // Total height consumed, in pixels.
  int height() const {
    int used = 0;
    for (int row = 0; row < static_cast<int>(rows_.size()); ++row) {
      if (std::any_of(rows_[row].begin(), rows_[row].end(),
                      [](bool b) { return b; })) {
        used = row + 1;
      }
    }
    return used == 0 ? 0 : used * cell_ + (used - 1) * gap_;
  }

 private:
  void ensureRows(int count) {
    while (static_cast<int>(rows_.size()) < count) {
      rows_.emplace_back(columns_, false);
    }
  }
  bool fits(int row, int column, int width, int height) const {
    for (int r = row; r < row + height; ++r) {
      for (int c = column; c < column + width; ++c) {
        if (rows_[r][c]) return false;
      }
    }
    return true;
  }
  void occupy(int row, int column, int width, int height) {
    for (int r = row; r < row + height; ++r) {
      for (int c = column; c < column + width; ++c) rows_[r][c] = true;
    }
  }

  int columns_;
  int cell_;
  int gap_;
  std::vector<std::vector<bool>> rows_;
};

}  // namespace

StartMenu::StartMenu(const ui::Theme& theme) : theme_(theme) {
  apps_ = sampleApps();
  groups_ = sampleTileGroups();
}

void StartMenu::setApps(std::vector<StartApp> apps) {
  apps_ = std::move(apps);
  invalidate();
}

void StartMenu::setTileGroups(std::vector<StartTileGroup> groups) {
  groups_ = std::move(groups);
  invalidate();
}

std::vector<StartApp> StartMenu::sampleApps() {
  return {
      {"Access", Color::rgb(0xA4373A)},
      {"Alarms & Clock", Color::rgb(0x0078D7)},
      {"Calculator", Color::rgb(0x4C4A48)},
      {"Calendar", Color::rgb(0x0078D7)},
      {"Camera", Color::rgb(0x4C4A48)},
      {"Cortana", Color::rgb(0x0078D7)},
      {"Excel", Color::rgb(0x217346)},
      {"Feedback Hub", Color::rgb(0x0078D7)},
      {"GitHub, Inc", Color::rgb(0xE8B339)},
      {"Groove Music", Color::rgb(0xE8622D)},
      {"Mail", Color::rgb(0x0078D7)},
  };
}

std::vector<StartTileGroup> StartMenu::sampleTileGroups() {
  // The default arrangement of a stock Windows 10 install. Tile order is
  // significant: the packer places in declaration order, so this is what
  // produces three mediums per row.
  return {
      {"Productivity",
       {
           {"Office", TileSize::Medium, Color::rgb(0xD83B01), {}, false, {}},
           // The Office folder tile: eight apps collapsed into one tile.
           {"Office apps", TileSize::Medium, Color::rgb(0xE8EBEE), {}, false,
            {{"O", Color::rgb(0xD83B01)},
             {"W", Color::rgb(0x2B579A)},
             {"X", Color::rgb(0x217346)},
             {"C", Color::rgb(0x0078D7)},
             {"P", Color::rgb(0xD24726)},
             {"N", Color::rgb(0x7719AA)},
             {"T", Color::rgb(0x6264A7)},
             {"S", Color::rgb(0x00AFF0)}}},
           {"Mail", TileSize::Medium, Color::rgb(0x0078D7),
            "See all your mail in one place", false, {}},
           {"Microsoft Edge", TileSize::Medium, Color::rgb(0x0078D7), {}, false,
            {}},
           {"Photos", TileSize::Medium, Color::rgb(0x0078D7), {}, false, {}},
           {"Calendar", TileSize::Medium, Color::rgb(0xE8EBEE), "Thursday 25",
            true, {}},
       }},
      {"Explore",
       {
           {"News", TileSize::Medium, Color::rgb(0xCC2027), {}, false, {}},
           {"Tips", TileSize::Medium, Color::rgb(0x0078D7), {}, false, {}},
           {"Movies & TV", TileSize::Medium, Color::rgb(0x0078D7), {}, false,
            {}},
           {"Your Phone", TileSize::Medium, Color::rgb(0x0078D7), {}, false, {}},
           {"Solitaire", TileSize::Medium, Color::rgb(0xE8EBEE), {}, false, {}},
           {"Weather", TileSize::Medium, Color::rgb(0x3A6EA5), "Mostly Sunny",
            true, {}},
       }},
      {"Build",
       {
           {"Terminal", TileSize::Medium, Color::rgb(0x1F1F1F), {}, false, {}},
           {"To Do", TileSize::Medium, Color::rgb(0x2564CF), {}, false, {}},
           {"Code", TileSize::Small, Color::rgb(0x0078D7), {}, false, {}},
           {"Remote", TileSize::Small, Color::rgb(0x0078D7), {}, false, {}},
           {"GitHub", TileSize::Small, Color::rgb(0x24292E), {}, false, {}},
           {"Calculator", TileSize::Small, Color::rgb(0x4C4A48), {}, false, {}},
       }},
  };
}

int StartMenu::preferredHeight(const ui::Theme& theme) const {
  const ui::Metrics& m = theme.metrics;
  int height = theme.scaled(m.startMenuTilePadding);
  for (const StartTileGroup& group : groups_) {
    TileGrid grid(m.tileGridColumns, theme.scaled(m.tileCell),
                  theme.scaled(m.tileGap));
    for (const StartTile& tile : group.tiles) grid.place(tile.size);
    height += theme.scaled(m.startGroupHeaderHeight) + grid.height() +
              theme.scaled(m.tileGap);
  }
  return height + theme.scaled(m.startMenuTilePadding);
}

void StartMenu::onPaint(PaintContext& ctx) {
  const ui::Metrics& m = ctx.theme.metrics;
  const Rect box = localBounds();

  const int railWidth = ctx.theme.scaled(m.startMenuRailWidth);
  const int listWidth = ctx.theme.scaled(m.startMenuAppListWidth);

  // One blur for the whole flyout, then three tints over it. Blurring each
  // column separately would show a seam at their boundaries.
  ui::blurBehind(ctx, box);
  ctx.fill(box, ctx.theme.surface(ctx.theme.palette.startMenuBackground,
                                  ctx.theme.palette.startMenuAcrylic));

  paintRail(ctx, {box.x, box.y, railWidth, box.h});
  paintAppList(ctx, {box.x + railWidth, box.y, listWidth, box.h});
  paintTilePanel(ctx, {box.x + railWidth + listWidth, box.y,
                       box.w - railWidth - listWidth, box.h});

  // Windows draws a hairline around the flyout; without it the menu bleeds
  // into a light wallpaper.
  ctx.canvas.strokeRectInside(ctx.toSurface(box),
                              ctx.theme.palette.startMenuBorder, 1);
}

void StartMenu::paintRail(PaintContext& ctx, Rect box) const {
  const ui::Metrics& m = ctx.theme.metrics;
  ctx.fill(box, ctx.theme.surface(ctx.theme.palette.startMenuRail,
                                  ctx.theme.palette.startMenuRailAcrylic));

  const int icon = ctx.theme.scaled(m.startRailIconSize);
  const int slot = ctx.theme.scaled(44);
  const Color ink = ctx.theme.palette.startMenuText;
  const int x = box.x + (box.w - icon) / 2;

  // Hamburger pinned to the top; the rest anchored to the bottom, which is how
  // Windows keeps Power in the same place regardless of menu height.
  const Rect hamburgerBox{x, box.y + (slot - icon) / 2, icon, icon};
  glyphs::hamburger(ctx.canvas, ctx.toSurface(hamburgerBox), ink);

  using Painter = void (*)(gfx::Canvas&, Rect, Color);
  const Painter bottom[] = {glyphs::user, glyphs::document, glyphs::picture,
                            glyphs::settings, glyphs::power};
  const int count = static_cast<int>(std::size(bottom));
  int y = box.bottom() - count * slot;
  for (Painter paint : bottom) {
    paint(ctx.canvas, ctx.toSurface({x, y + (slot - icon) / 2, icon, icon}),
          ink);
    y += slot;
  }
}

void StartMenu::paintAppList(PaintContext& ctx, Rect box) const {
  const ui::Metrics& m = ctx.theme.metrics;
  const ui::Palette& palette = ctx.theme.palette;

  const int rowHeight = ctx.theme.scaled(m.startAppRowHeight);
  const int headerHeight = ctx.theme.scaled(m.startLetterHeaderHeight);
  const int iconSize = ctx.theme.scaled(m.startAppIconSize);
  const int leftPad = ctx.theme.scaled(16);

  int y = box.y + ctx.theme.scaled(8);
  char currentLetter = '\0';

  for (const StartApp& app : apps_) {
    const char letter = initialOf(app.name);
    if (letter != currentLetter) {
      currentLetter = letter;
      if (y + headerHeight > box.bottom()) break;
      // Letter headers are jump-list anchors in Windows; they are drawn in the
      // accent colour and indented to align with the app labels.
      ctx.label({box.x + leftPad, y, box.w - leftPad, headerHeight},
                std::string(1, letter), palette.accent, HAlign::Left,
                VAlign::Middle);
      y += headerHeight;
    }

    if (y + rowHeight > box.bottom()) break;

    const Rect iconRect{box.x + leftPad, y + (rowHeight - iconSize) / 2,
                        iconSize, iconSize};
    glyphs::appPlaceholder(ctx.canvas, ctx.toSurface(iconRect), app.color);
    ctx.label(iconRect, app.name.substr(0, 1), Color::rgb(0xFFFFFF),
              HAlign::Center, VAlign::Middle);

    const int labelX = iconRect.right() + ctx.theme.scaled(14);
    ctx.label({labelX, y, box.right() - labelX - ctx.theme.scaled(8), rowHeight},
              app.name, palette.startMenuText, HAlign::Left, VAlign::Middle);
    y += rowHeight;
  }
}

void StartMenu::paintTilePanel(PaintContext& ctx, Rect box) const {
  const ui::Metrics& m = ctx.theme.metrics;
  const ui::Palette& palette = ctx.theme.palette;

  ctx.fill(box, ctx.theme.surface(palette.startMenuTilePanel,
                                  palette.startMenuTilePanelAcrylic));

  const int padding = ctx.theme.scaled(m.startMenuTilePadding);
  const int cell = ctx.theme.scaled(m.tileCell);
  const int gap = ctx.theme.scaled(m.tileGap);
  const int headerHeight = ctx.theme.scaled(m.startGroupHeaderHeight);

  int y = box.y + padding;
  for (const StartTileGroup& group : groups_) {
    if (y + headerHeight > box.bottom()) break;
    ctx.label({box.x + padding, y, box.w - padding * 2, headerHeight},
              group.name, palette.startMenuSubtleText, HAlign::Left,
              VAlign::Middle);
    y += headerHeight;

    TileGrid grid(m.tileGridColumns, cell, gap);
    for (const StartTile& tile : group.tiles) {
      const Rect slot = grid.place(tile.size);
      const Rect rect{box.x + padding + slot.x, y + slot.y, slot.w, slot.h};
      // Tiles that run past the bottom are drawn and clipped, not skipped:
      // skipping would let a later, shorter tile jump ahead of one that
      // overflowed, silently reordering the last visible row.
      if (rect.y >= box.bottom()) continue;

      ctx.fill(rect, tile.color);

      // Folder tiles show a grid of miniature app icons over the panel colour
      // rather than one brand fill.
      if (!tile.folder.empty()) {
        const int columns = 3;
        const int inset = ctx.theme.scaled(10);
        const int spacing = ctx.theme.scaled(4);
        const int mini =
            (rect.w - inset * 2 - spacing * (columns - 1)) / columns;
        for (size_t i = 0; i < tile.folder.size(); ++i) {
          const int column = static_cast<int>(i) % columns;
          const int row = static_cast<int>(i) / columns;
          const Rect cellRect{rect.x + inset + column * (mini + spacing),
                              rect.y + inset + row * (mini + spacing), mini,
                              mini};
          if (cellRect.bottom() > rect.bottom() - inset / 2) break;
          ctx.fill(cellRect, tile.folder[i].color);
          ctx.label(cellRect, tile.folder[i].label, Color::rgb(0xFFFFFF),
                    HAlign::Center, VAlign::Middle);
        }
        continue;
      }

      // A light tile needs dark text; the brand-coloured ones are all dark
      // enough for white. Deciding per tile beats a per-tile colour field
      // that callers would forget to set.
      const bool lightTile =
          (static_cast<int>(tile.color.r) + tile.color.g + tile.color.b) > 600;
      const Color ink =
          lightTile ? Color::rgb(0x000000) : Color::rgb(0xFFFFFF);

      // Small tiles have no room for text; Windows shows the icon alone.
      if (tile.size == TileSize::Small) {
        ctx.label(rect, tile.label.substr(0, 1), ink, HAlign::Center,
                  VAlign::Middle);
        continue;
      }

      if (!tile.caption.empty()) {
        ctx.label({rect.x + ctx.theme.scaled(8), rect.y + ctx.theme.scaled(6),
                   rect.w - ctx.theme.scaled(16), ctx.theme.scaled(34)},
                  tile.caption, ink, HAlign::Left, VAlign::Top);
      }

      const int labelHeight = ctx.theme.scaled(18);
      ctx.label({rect.x + ctx.theme.scaled(8),
                 rect.bottom() - labelHeight - ctx.theme.scaled(4),
                 rect.w - ctx.theme.scaled(16), labelHeight},
                tile.label, ink, HAlign::Left, VAlign::Middle);
    }
    y += grid.height() + gap * 2;
  }
}

}  // namespace finux::shell

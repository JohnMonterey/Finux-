#pragma once

#include <string>
#include <vector>

#include "ui/widget.hpp"

namespace finux::shell {

using ui::PaintContext;
using ui::Widget;

// Tile footprints, in grid cells. Windows exposes exactly these four.
enum class TileSize {
  Small,   // 1x1
  Medium,  // 2x2
  Wide,    // 4x2
  Large,   // 4x4
};

// One of the small application icons inside a folder tile.
struct SubIcon {
  std::string label;
  Color color;
};

struct StartTile {
  std::string label;
  TileSize size = TileSize::Medium;
  // Brand colour of the application. Real tiles paint the app's own artwork;
  // until an icon source exists this is the tile's whole appearance.
  Color color = Color::rgb(0x0078D7);
  // Extra text some tiles carry (Mail's "See all your mail in one place").
  std::string caption;
  // Tiles that show their label inside the tile body rather than beneath it.
  bool labelInside = false;
  // A "folder" tile: a group of applications collapsed into one tile, drawn as
  // a grid of miniature icons over the panel colour instead of a single brand
  // fill. Windows creates these when tiles are dragged on top of each other.
  std::vector<SubIcon> folder;
};

struct StartTileGroup {
  std::string name;
  std::vector<StartTile> tiles;
};

struct StartApp {
  std::string name;
  Color color = Color::rgb(0x0078D7);
};

// The Start menu.
//
// Three columns, in the order Windows draws them: a narrow icon rail, the
// alphabetical "all apps" list, and the tile panel. The rail and list share a
// background; the tile panel sits on its own slightly different surface, which
// is what gives the menu its two-tone look.
class StartMenu final : public Widget {
 public:
  explicit StartMenu(const ui::Theme& theme);

  void setApps(std::vector<StartApp> apps);
  void setTileGroups(std::vector<StartTileGroup> groups);

  // The reference set from a stock Windows 10 install, for rendering and for
  // tests. Real entries come from XDG .desktop files.
  static std::vector<StartApp> sampleApps();
  static std::vector<StartTileGroup> sampleTileGroups();

  // Height the tile panel needs for the current groups, so the menu can size
  // itself to content the way Windows does.
  int preferredHeight(const ui::Theme& theme) const;

 protected:
  void onPaint(PaintContext& ctx) override;
  bool interactive() const override { return true; }

 private:
  void paintRail(PaintContext& ctx, Rect box) const;
  void paintAppList(PaintContext& ctx, Rect box) const;
  void paintTilePanel(PaintContext& ctx, Rect box) const;

  const ui::Theme& theme_;
  std::vector<StartApp> apps_;
  std::vector<StartTileGroup> groups_;
};

// Cells occupied by a tile size, as (width, height) in grid cells.
Size tileCells(TileSize size);

}  // namespace finux::shell

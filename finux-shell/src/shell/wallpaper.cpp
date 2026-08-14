#include "shell/wallpaper.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace finux::shell {
namespace {

// Geometry of the backdrop, expressed as fractions of the screen so the
// composition holds at any resolution.
struct Layout {
  Point vanishing;   // where the light comes from
  Point paneOrigin;  // top-left of the window shape
  int paneWidth;
  int paneHeight;
  int mullion;  // gap between panes
};

Layout layoutFor(Rect screen) {
  Layout l;
  // The window sits right of centre, the light source just beyond its top-right
  // corner -- the arrangement that makes the rays sweep left across the frame.
  l.paneWidth = screen.w * 10 / 100;
  l.paneHeight = screen.h * 16 / 100;
  l.mullion = std::max(2, screen.w / 300);
  l.paneOrigin = {screen.x + screen.w * 64 / 100, screen.y + screen.h * 37 / 100};
  l.vanishing = {l.paneOrigin.x + l.paneWidth * 2 + l.mullion,
                 l.paneOrigin.y - screen.h * 2 / 100};
  return l;
}

// A ray is a thin wedge from the light source sweeping away across the frame.
void paintRay(gfx::Canvas& canvas, Point from, double angleDeg, int length,
              int spread, Color color) {
  constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
  const double a = angleDeg * kDegToRad;
  const auto endX = static_cast<int>(from.x + std::cos(a) * length);
  const auto endY = static_cast<int>(from.y + std::sin(a) * length);
  const auto normalX = static_cast<int>(-std::sin(a) * spread);
  const auto normalY = static_cast<int>(std::cos(a) * spread);

  canvas.fillPolygonGradient(
      {from,
       {endX + normalX, endY + normalY},
       {endX - normalX, endY - normalY}},
      from, {endX, endY}, color, color.withAlpha(0));
}

}  // namespace

void paintProceduralBackdrop(gfx::Canvas& canvas, Rect screen) {
  if (screen.empty()) return;
  const Layout l = layoutFor(screen);

  // --- field ---------------------------------------------------------------
  // Near-black at the edges, deep blue around the light source.
  canvas.fillRect(screen, Color::rgb(0x03060E));
  canvas.fillRadialGradient(screen, l.vanishing,
                            static_cast<int>(screen.w * 0.85),
                            Color::rgb(0x1D6FC4), Color::rgba(0x03060E, 0x00));
  canvas.fillRadialGradient(screen, l.vanishing,
                            static_cast<int>(screen.w * 0.35),
                            Color::rgba(0x4FA9F0, 0x66),
                            Color::rgba(0x0A63B0, 0x00));

  // --- rays ----------------------------------------------------------------
  // Swept leftward and fanned slightly, brightest nearest the source.
  const int rayLength = screen.w;
  const Color ray = Color::rgba(0x8FD0FF, 0x4D);
  paintRay(canvas, l.vanishing, 196.0, rayLength, screen.h / 26, ray);
  paintRay(canvas, l.vanishing, 187.0, rayLength, screen.h / 40,
           ray.withAlpha(0x66));
  paintRay(canvas, l.vanishing, 178.0, rayLength, screen.h / 90,
           ray.withAlpha(0x80));
  paintRay(canvas, {l.vanishing.x, l.vanishing.y + l.paneHeight * 2},
           168.0, rayLength, screen.h / 34, ray.withAlpha(0x40));

  // --- panes ---------------------------------------------------------------
  // Four quadrilaterals in perspective: the further edge is shorter, which is
  // what reads as depth rather than as a flat grid.
  const int w = l.paneWidth;
  const int h = l.paneHeight;
  const int m = l.mullion;
  const int skew = h / 6;  // how much the far edge rises

  for (int row = 0; row < 2; ++row) {
    for (int column = 0; column < 2; ++column) {
      const int x = l.paneOrigin.x + column * (w + m);
      const int y = l.paneOrigin.y + row * (h + m);
      // Left edge is further away, so it is inset vertically.
      const int leftInset = (column == 0) ? skew : skew / 2;
      const int rightInset = (column == 0) ? skew / 2 : 0;

      const std::vector<Point> pane = {
          {x, y + leftInset},
          {x + w, y + rightInset},
          {x + w, y + h - rightInset},
          {x, y + h - leftInset},
      };

      canvas.fillPolygonGradient(pane, {x, y}, {x + w, y + h},
                                 Color::rgba(0x2E93E8, 0xE6),
                                 Color::rgba(0x8FD3FF, 0xF2));

      // Bright rims: the light spilling around the frame.
      canvas.strokeLine(pane[0], pane[1], 2.0, Color::rgba(0xFFFFFF, 0xCC));
      canvas.strokeLine(pane[3], pane[2], 2.0, Color::rgba(0xFFFFFF, 0x99));
    }
  }

  // --- mullion band --------------------------------------------------------
  // The horizontal frame member, thrown as a dark bar clear across the frame.
  // Without it the composition reads as a glowing rectangle rather than as
  // light coming through a window.
  {
    const int bandY = l.paneOrigin.y + h + m / 2;
    const int bandHeight = std::max(2, screen.h / 110);
    canvas.fillPolygonGradient(
        {{screen.x, bandY - bandHeight / 2 - screen.h / 60},
         {l.paneOrigin.x + w * 2, bandY - bandHeight},
         {l.paneOrigin.x + w * 2, bandY + bandHeight},
         {screen.x, bandY + bandHeight / 2 + screen.h / 60}},
        {l.paneOrigin.x, bandY}, {screen.x, bandY},
        Color::rgba(0x02060C, 0xE6), Color::rgba(0x02060C, 0x33));
  }

  // --- bloom ---------------------------------------------------------------
  canvas.fillRadialGradient(
      {l.vanishing.x - screen.w / 8, l.vanishing.y - screen.h / 8,
       screen.w / 4, screen.h / 4},
      l.vanishing, screen.w / 12, Color::rgba(0xFFFFFF, 0xB3),
      Color::rgba(0xFFFFFF, 0x00));

  // Haze across the lower half, so the acrylic blur has texture to pick up.
  canvas.fillLinearGradient({screen.x, screen.y + screen.h / 2, screen.w,
                             screen.h / 2},
                            {screen.x, screen.y + screen.h / 2},
                            {screen.x, screen.bottom()},
                            Color::rgba(0x0A63B0, 0x00),
                            Color::rgba(0x020509, 0x99));
}

bool paintWallpaperFile(gfx::Canvas& canvas, Rect screen,
                        const std::string& path, Fit fit) {
  if (path.empty() || screen.empty()) return false;

  gfx::Image source = gfx::Image::load(path);
  if (!source.valid()) return false;

  const double scaleX = static_cast<double>(screen.w) / source.width();
  const double scaleY = static_cast<double>(screen.h) / source.height();

  double scale = 1.0;
  switch (fit) {
    case Fit::Cover: scale = std::max(scaleX, scaleY); break;
    case Fit::Contain: scale = std::min(scaleX, scaleY); break;
    case Fit::Center: scale = 1.0; break;
    case Fit::Stretch:
    case Fit::Tile: break;
  }

  if (fit == Fit::Stretch) {
    // Letterbox colour still matters here: a stretch that does not quite cover
    // due to rounding would otherwise leave uninitialised pixels.
    canvas.fillRect(screen, Color::rgb(0x000000));
    canvas.blitScaled(screen, source);
    return true;
  }

  if (fit == Fit::Tile) {
    for (int y = screen.y; y < screen.bottom(); y += source.height()) {
      for (int x = screen.x; x < screen.right(); x += source.width()) {
        canvas.blit({x, y}, source);
      }
    }
    return true;
  }

  const auto width = static_cast<int>(source.width() * scale + 0.5);
  const auto height = static_cast<int>(source.height() * scale + 0.5);
  const Point at{screen.x + (screen.w - width) / 2,
                 screen.y + (screen.h - height) / 2};

  // Contain and Center can leave bars; fill first so they are black rather
  // than whatever the surface happened to contain.
  if (fit != Fit::Cover) canvas.fillRect(screen, Color::rgb(0x000000));
  if (fit == Fit::Center) {
    canvas.blit(at, source);
  } else {
    canvas.blitScaled({at.x, at.y, width, height}, source);
  }
  return true;
}

void paintDesktop(gfx::Canvas& canvas, Rect screen, const std::string& path,
                  Fit fit) {
  if (paintWallpaperFile(canvas, screen, path, fit)) return;
  paintProceduralBackdrop(canvas, screen);
}

}  // namespace finux::shell

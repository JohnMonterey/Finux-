#pragma once

#include <blend2d/blend2d.h>

#include <vector>

#include "core/color.hpp"
#include "core/geometry.hpp"
#include "gfx/image.hpp"

namespace finux::gfx {

// Thin integer-rect drawing surface over Blend2D.
//
// The API is deliberately narrow and pixel-snapped. Windows 10's shell chrome
// is almost entirely axis-aligned rectangles, 1px hairlines and text; exposing
// a general vector API here would invite half-pixel geometry, which is exactly
// the class of error the pixel-diff tests exist to catch.
class Canvas {
 public:
  explicit Canvas(Image& target);
  ~Canvas();

  Canvas(const Canvas&) = delete;
  Canvas& operator=(const Canvas&) = delete;

  // Ends the underlying context so the target's pixels can be read or written
  // directly -- which the glyph blitter does, because subpixel coverage cannot
  // go through a single-alpha paint pipeline.
  //
  // The context is transparently re-established by the next drawing call, so
  // callers may interleave Canvas operations and direct pixel access freely.
  // Getting this wrong is not a subtle failure: an ended BLContext silently
  // discards every subsequent draw, so a single stray text call used to make
  // the entire rest of the frame vanish.
  void syncPixels();

  // Finishes drawing. Called by the destructor.
  void flush() { syncPixels(); }

  void clear(Color c);
  void fillRect(Rect r, Color c);

  // A 1px border drawn *inside* r, the way Win10 control borders sit.
  void strokeRectInside(Rect r, Color c, int thickness = 1);

  // Single-pixel hairlines. Separate from fillRect only for readability at
  // call sites, where "the 1px line under an active task button" is a concept.
  void hLine(int x, int y, int length, Color c);
  void vLine(int x, int y, int length, Color c);

  void fillRoundedRect(Rect r, double radius, Color c);

  // Two-stop gradients. The shell proper uses flat fills; these exist for the
  // desktop backdrop, where a single colour reads as obviously fake.
  void fillLinearGradient(Rect r, Point from, Point to, Color a, Color b);
  void fillRadialGradient(Rect r, Point center, int radius, Color inner,
                          Color outer);

  // Arbitrary polygon, for backdrop geometry that is not axis-aligned.
  void fillPolygon(const std::vector<Point>& points, Color c);
  void fillPolygonGradient(const std::vector<Point>& points, Point from,
                           Point to, Color a, Color b);

  // Glyph primitives.
  //
  // The shell draws its own chrome glyphs -- the search magnifier, the Cortana
  // ring, the power symbol, the settings gear -- because the Windows icon set
  // is not redistributable (see docs/ASSETS.md). These are the shapes those
  // glyphs are built from. Centres and radii are integral for the same reason
  // the rest of the geometry is: a half-pixel circle centre reads as a blurry
  // smudge at 16px.
  void fillCircle(Point center, int radius, Color c);
  void strokeCircle(Point center, int radius, double thickness, Color c);
  void strokeArc(Point center, int radius, double startDeg, double sweepDeg,
                 double thickness, Color c);
  void strokeLine(Point a, Point b, double thickness, Color c);
  void fillTriangle(Point a, Point b, Point p3, Color c);

  void blit(Point at, const Image& src);
  void blit(Point at, const Image& src, Rect srcRect);
  // Scales `src` to fill `dest`. Used for wallpaper, which almost never
  // matches the screen resolution exactly.
  void blitScaled(Rect dest, const Image& src);

  // Clipping is a stack so widgets can nest without knowing their parents.
  void pushClip(Rect r);
  void popClip();
  Rect clip() const { return clip_; }

  Image& target() { return target_; }

 private:
  // Re-establishes the context if a syncPixels() ended it, restoring comp op
  // and clip. Every drawing entry point calls this first.
  void ensureContext();
  void applyClip();

  Image& target_;
  BLContext ctx_;
  Rect clip_;
  std::vector<Rect> clipStack_;
  bool active_ = false;
};

}  // namespace finux::gfx

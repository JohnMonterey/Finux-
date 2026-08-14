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

  void blit(Point at, const Image& src);
  void blit(Point at, const Image& src, Rect srcRect);

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

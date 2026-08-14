#pragma once

#include "gfx/effects.hpp"
#include "ui/widget.hpp"

namespace finux::ui {

// Blurs whatever is already in the surface beneath `local`, so the widget can
// then fill with a translucent tint.
//
// The two halves are separate on purpose: several tints may sit over one blur
// (the Start menu's rail, app list and tile panel do exactly that), and
// blurring each separately would leave a seam where their edges meet.
//
// Does nothing when transparency effects are off, which is both the Windows
// setting and what tests want -- a blurred backdrop makes exact colour
// assertions meaningless.
inline void blurBehind(PaintContext& ctx, Rect local) {
  if (!ctx.theme.transparencyEffects) return;
  // Direct pixel access, so the paint context has to be stood down first.
  ctx.canvas.syncPixels();
  gfx::effects::blurBackdrop(ctx.canvas.target(), ctx.toSurface(local),
                             ctx.theme.scaled(ctx.theme.acrylicBlurRadius),
                             ctx.theme.acrylicNoise);
}

}  // namespace finux::ui

#pragma once

#include "core/color.hpp"
#include "core/geometry.hpp"
#include "gfx/canvas.hpp"

namespace finux::ui::glyphs {

// Shell chrome glyphs, drawn as vectors.
//
// Windows renders these from Segoe MDL2 Assets, a font that ships with the OS
// and is not redistributable (docs/ASSETS.md). Rather than depend on an icon
// font the user may not have, the shell draws the handful of shapes it needs.
//
// Each function fills `box` -- a square is expected; callers pass the icon's
// layout rect and the glyph centres itself inside it. Sizes are integral, so
// the same glyph at 16px and 20px stays crisp rather than being scaled.

void windowsLogo(gfx::Canvas& canvas, Rect box, Color color);
void hamburger(gfx::Canvas& canvas, Rect box, Color color);
void search(gfx::Canvas& canvas, Rect box, Color color);
void cortanaRing(gfx::Canvas& canvas, Rect box, Color color);
void taskView(gfx::Canvas& canvas, Rect box, Color color);

// Start menu rail.
void user(gfx::Canvas& canvas, Rect box, Color color);
void document(gfx::Canvas& canvas, Rect box, Color color);
void picture(gfx::Canvas& canvas, Rect box, Color color);
void settings(gfx::Canvas& canvas, Rect box, Color color);
void power(gfx::Canvas& canvas, Rect box, Color color);

// Notification area.
void chevronUp(gfx::Canvas& canvas, Rect box, Color color);
void cloud(gfx::Canvas& canvas, Rect box, Color color);
void network(gfx::Canvas& canvas, Rect box, Color color);
void volume(gfx::Canvas& canvas, Rect box, Color color);
void actionCenter(gfx::Canvas& canvas, Rect box, Color color);

// Generic placeholder for an application whose real icon is unavailable.
// Draws a tinted tile; callers overlay the app's initial.
void appPlaceholder(gfx::Canvas& canvas, Rect box, Color color);

}  // namespace finux::ui::glyphs

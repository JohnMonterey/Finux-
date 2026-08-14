#pragma once

#include <string>

#include "gfx/canvas.hpp"
#include "gfx/image.hpp"

namespace finux::shell {

// The desktop backdrop.
//
// Windows 10's shipped wallpaper is Microsoft artwork and is not
// redistributable, so this module does two things: it loads whatever image the
// user points it at, and -- when none is supplied -- draws an original
// procedural backdrop in a comparable style so the shell has something to
// composite its acrylic surfaces against out of the box.
//
// The procedural one matters more than it sounds. Acrylic is a blur of what is
// behind it; against a flat colour the effect is invisible, so a plain fill
// would make the taskbar look wrong in exactly the way this is meant to fix.
enum class Fit {
  // Scale to cover, cropping the overflow. What Windows calls "Fill".
  Cover,
  // Scale to fit entirely, letterboxing. What Windows calls "Fit".
  Contain,
  Stretch,
  Center,
  Tile,
};

// Draws an original "light through a window" backdrop: a dark field, four
// perspective panes, and rays from the vanishing point. Deterministic.
void paintProceduralBackdrop(gfx::Canvas& canvas, Rect screen);

// Loads `path` and draws it into `screen` with the requested fit.
// Returns false if the file cannot be read or decoded, leaving the canvas
// untouched so the caller can fall back.
bool paintWallpaperFile(gfx::Canvas& canvas, Rect screen,
                        const std::string& path, Fit fit = Fit::Cover);

// Convenience: use the file when it loads, otherwise the procedural backdrop.
// An empty path goes straight to procedural.
void paintDesktop(gfx::Canvas& canvas, Rect screen, const std::string& path,
                  Fit fit = Fit::Cover);

}  // namespace finux::shell

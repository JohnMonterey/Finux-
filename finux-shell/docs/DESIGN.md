# Design

## Layering

```
src/core/       geometry, colour            no dependencies
src/gfx/        Image, Canvas               Blend2D
src/text/       FontLibrary, TextRenderer   FreeType, HarfBuzz, fontconfig
src/ui/         Widget, Theme               gfx + text
src/shell/      Taskbar, main               ui + platform
src/platform/   Platform, Window            XCB (Wayland later)
```

Dependencies point downward only. `src/platform` knows nothing about widgets
beyond the mouse-button enum, and `src/ui` knows nothing about X11 — which is
what keeps a Wayland backend a matter of adding a directory.

## Decisions worth knowing

### Integer geometry everywhere

The Windows shell is laid out on whole device pixels at every scale factor.
Floats in the geometry layer are how 1px seams appear between the taskbar and
the screen edge, so `Rect`/`Point`/`Size` are integral and `Theme::scaled()`
rounds explicitly.

### Text bypasses the rasteriser

Blend2D composites through a single alpha mask. ClearType-style output needs
independent red/green/blue coverage per pixel, so `TextRenderer` rasterises
with FreeType and writes into the surface itself.

That makes drawing and direct pixel access interleave, which `Canvas` handles
by suspending its `BLContext` on `syncPixels()` and reviving it — restoring
comp-op and the clip stack — on the next draw. An earlier version ended the
context permanently instead. Nothing crashed and nothing warned; every fill
issued after the first text draw was silently discarded, so widgets simply
stopped painting partway through the frame. `tests/test_canvas_interleave.cpp`
exists specifically to keep that from coming back.

### Windows-tuned glyph rendering

Three settings together account for most of "this looks like Windows":

| Setting | Value | Why |
| --- | --- | --- |
| Hinting | `FT_LOAD_TARGET_LIGHT` | Vertical-only. Full hinting snaps stems horizontally and immediately reads as "Linux" even with the right typeface. |
| Coverage | LCD subpixel + FreeType's default filter | ClearType-alike. Unfiltered LCD gives colour fringing. |
| Gamma | 1.8 | Linear coverage renders visibly thinner than Windows. |

Fallback is deliberate: subpixel silently degrades to grayscale if this
FreeType lacks LCD support, rather than shipping fringed text.

### Theme is the single source of truth

Every metric and colour lives in `src/ui/theme.hpp`. No widget hardcodes a
number. This is what makes a pixdiff failure actionable: one value moves and
every consumer follows.

The values in there are a *starting point*, not gospel. They were chosen to be
plausible and must be validated against reference captures. The harness is the
authority; the header is a hypothesis.

### The taskbar is a shell component, not a window that looks like one

Three EWMH capabilities carry that distinction:

- `_NET_WM_STRUT_PARTIAL` — maximised windows stop above the bar. The
  `*_start_x`/`*_end_x` fields are derived from the window geometry, not
  assumed, so a multi-monitor setup does not reserve space across every head.
- `_NET_CLIENT_LIST` + `_NET_ACTIVE_WINDOW` + `_NET_WM_STATE_HIDDEN` — the task
  button list, with the WM as the authoritative source. The shell re-reads the
  list on change rather than maintaining a diff.
- `WM_CLASS` — the stable key tying a running window to a pinned `.desktop`
  entry. The instance name varies with `argv[0]`; the class does not.

`_NET_WM_WINDOW_TYPE_DOCK` keeps the bar off the window list and above ordinary
windows without override-redirect, which would also cost keyboard focus and
WM-managed stacking.

**Size hints are not optional.** Without `WM_NORMAL_HINTS`, a window manager
treats the requested position as a suggestion: openbox relocates the bar to the
top of the screen while still honouring a bottom-edge strut, leaving the
reserved space and the bar on opposite edges. `USPosition` plus `min == max`
size states the geometry is deliberate. `tests/integration/x11_smoke.py` covers
this; no offscreen test can.

### Rebuild, don't diff

`Taskbar::setToplevels()` discards and rebuilds its children. The list is at
most a few dozen entries, and incremental diffing against the previous set is
reliably where panel implementations grow stale-button bugs.

### Layout runs at paint time

Task button and clock widths depend on measured text, and measurement needs the
font, which only the paint context carries. `Taskbar` therefore marks itself
dirty on resize and lays out lazily inside `onPaint`.

## Build notes

Blend2D is pinned to an exact commit rather than a branch. It publishes no
release tags, and `master` recently renamed its entire public API from
camelCase to snake_case (`writeToFile` → `write_to_file`), so tracking a branch
would break the tree under anyone who cloned it on a different day.

`FINUX_BLEND2D_JIT` defaults **off**. On CMake 3.28, configuring asmjit
alongside a JIT-enabled Blend2D drives `CheckCXXCompilerFlag` into unbounded
recursion — raising `CMAKE_MAXIMUM_RECURSION_DEPTH` converts the error into a
segfault, and either half configures cleanly alone, so it is an upstream
interaction bug. The portable reference pipeline is comfortably fast enough for
shell chrome. Measure before caring.

## The Start menu

Three columns, drawn in the order Windows draws them: an icon rail, the
alphabetical app list, and the tile panel. The rail and list share a
background; the panel sits on its own slightly different surface, which is
what gives the menu its two-tone look.

The tile grid is a first-fit packer over six cells. A "cell" is the small-tile
footprint; medium tiles are 2x2 cells, wide 4x2, large 4x4, each absorbing the
gutters it spans. The panel width is *derived* from the grid rather than chosen
independently -- it exists to hold exactly six cells across -- so three medium
tiles fill a row exactly. `test_startmenu_render` asserts that arithmetic
directly, because an off-by-one in the gutter count produces a menu that looks
plausible in isolation while matching no Windows layout at all.

Windows lets users drag tiles anywhere and stores explicit positions. Until
there is a tile-layout store to read, packing in declaration order reproduces
the default arrangement, which is what a fresh install shows.

Tiles that run past the bottom edge are drawn and clipped rather than skipped.
Skipping let a later, shorter tile jump ahead of one that overflowed, silently
reordering the last visible row.

## Acrylic

The taskbar and Start menu are translucent, and that is a composition rather
than an alpha value:

1. **Blur** what is behind the surface (three box passes ≈ Gaussian, on
   premultiplied pixels so transparent pixels cannot drag colour into their
   neighbours).
2. **Tint** the blurred result, around 85% coverage.
3. **Noise**, a few levels, so large flat areas do not band.

Skipping step 1 and just lowering alpha is the usual way a Windows-alike gets
this wrong: wallpaper detail shows straight through, and text over a busy
background becomes unreadable — which is precisely what the blur prevents.

Blur and tint are separate calls because one blur may carry several tints. The
Start menu's rail, app list and tile panel are three tints over a single
blurred backdrop; blurring each separately leaves a seam at their boundaries.

The blur's edge handling clamps rather than zero-pads. Zero-padding darkens
the borders, which on a taskbar reads as a drop shadow along every edge —
`test_effects` asserts a flat field survives the blur unchanged.

`Theme::transparencyEffects` mirrors the Windows setting of the same name.
With it off the shell paints flat palette colours and skips the blur entirely,
which is also what the structural tests use: exact colour assertions are
meaningless over a blurred backdrop. The acrylic path has its own coverage in
`test_effects`, and `x11_smoke` asserts the on-screen bar is *not* the flat
tint — a check that fails if acrylic silently stops sampling the backdrop.

### Pseudo-transparency, for now

On X11 the bar composites the *wallpaper* behind itself, not whatever windows
happen to be there. For a static wallpaper the result is identical to real
transparency; seeing through to other windows needs XComposite to sample the
screen, which is a later stage.

## The wallpaper

Windows' shipped wallpaper is Microsoft artwork, so the shell loads whatever
image it is pointed at (anything Blend2D decodes: PNG, JPEG, BMP) with Windows'
fit modes — Fill, Fit, Stretch, Tile, Center — and otherwise draws an original
procedural backdrop.

That fallback is not decoration. Acrylic is a blur of what is behind it;
against a flat colour the effect is invisible, so a plain fill would make the
shell look wrong in exactly the way acrylic exists to fix.

## Themes

Light is the default, because that is what Windows 10 ships. The two palettes
differ in more than lightness: the hover and press layers are translucent
*white* over a dark bar and translucent *black* over a light one, and the Start
button takes the accent colour only in the light theme. Storing those layers
with alpha rather than pre-flattening them is what lets one widget draw
correctly under both.

## What is deliberately missing

Placeholders that must be replaced, and are marked as such in the source:

- The Start button glyph is a generic four-pane shape, not the Windows logo.
- Task button icons are tinted tiles with the app's initial. Real icons come
  from `_NET_WM_ICON` or the XDG icon theme via the `.desktop` entry matching
  `WM_CLASS`.
- Taskbar transparency composites over a flat colour; the blurred backdrop
  needs XComposite.
- The clock formats en-US only. Locale handling belongs with the settings layer.

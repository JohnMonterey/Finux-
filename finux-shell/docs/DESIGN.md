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

## What is deliberately missing

Placeholders that must be replaced, and are marked as such in the source:

- The Start button glyph is a generic four-pane shape, not the Windows logo.
- Task button icons are tinted tiles with the app's initial. Real icons come
  from `_NET_WM_ICON` or the XDG icon theme via the `.desktop` entry matching
  `WM_CLASS`.
- Taskbar transparency composites over a flat colour; the blurred backdrop
  needs XComposite.
- The clock formats en-US only. Locale handling belongs with the settings layer.

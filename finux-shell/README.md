# finux-shell

A natively written C++ replica of the Windows 10 shell for Linux — the taskbar
first, `explorer.exe`'s file-manager window after that — rendered pixel by
pixel rather than themed.

**Status: stages 1–2 of 7.** The rendering foundation, the widget core, the
Windows-tuned text stack, the X11 backend and the fidelity harness exist and
are tested. The bar draws, docks, reserves screen space and lists real windows.
The Start menu, context menus, system tray and Explorer window do not exist yet.

## Why not just theme GTK or Qt

Every "Windows 10 on Linux" project — Q4OS's theme, LinuxFX, the various
GTK/Kvantum skins — is a theme over a toolkit, and they all plateau at "close,
but obviously not it." That is structural, not a matter of effort: GTK and Qt
own widget metrics, focus rings, scrollbar geometry, menu shadows and text
baselines. A theme can restyle colours; it cannot produce the 1px inner border
on a Win10 combo box or the exact hover rectangle on the Start button.

So this project owns the whole pipeline:

```
C++20  →  widget tree  →  Blend2D rasteriser  →  X11 (XCB)
                       ↘  FreeType + HarfBuzz glyph blitter
```

No GTK, no Qt, no theme engine. Text deliberately bypasses Blend2D, because
ClearType-style output needs independent per-channel coverage that a
single-alpha paint pipeline cannot express.

## Building

Requires a C++20 compiler, CMake ≥ 3.20, and:

```sh
sudo apt install \
  libxcb-ewmh-dev libxcb-icccm4-dev libxcb-image0-dev libxcb-shm0-dev \
  libxcb-randr0-dev libxcb-composite0-dev \
  libfreetype-dev libharfbuzz-dev libfontconfig-dev
```

Blend2D is fetched and built automatically, pinned to an exact commit.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/src/finux-shell
```

Run it under any EWMH-compliant window manager (openbox, xfwm, i3, …).

## Testing

```sh
cd build && ctest --output-on-failure
```

| Test | Covers |
| --- | --- |
| `test_geometry` | Rect/colour algebra, DPI scaling at 100/125/150/175% |
| `test_pixdiff` | The fidelity comparator itself |
| `test_canvas_interleave` | Drawing interleaved with direct pixel access |
| `test_text` | Shaping, rasterisation, clipping, UTF-8-safe ellipsis |
| `test_taskbar_render` | Offscreen taskbar layout and structure |
| `x11_smoke` | Real WM: strut reservation, placement, on-screen pixels |

Tests that cannot run (no fonts, no X server, no `openbox`) report as
**skipped**, never as passed — an under-provisioned CI box must not show green.

## How "looks exactly like Windows 10" becomes a test

Fidelity is not settled by argument or by eye. `tests/pixdiff` compares
rendered output against captured Windows 10 screenshots per pixel in CIE L\*a\*b\*
(not RGB — the taskbar is near-black, where equal RGB steps are far more
visible than the same steps in midtones), then fails on either too many
differing pixels or any single pixel being too far off, and writes a magenta
diff image showing where.

Reference captures are **not** in this repository — screenshots of the Windows
UI are Microsoft's. Supply your own:

1. Run the suite once; it writes what it rendered to
   `tests/pixdiff/output/<name>.actual.png` and tells you the reference is missing.
2. Capture the same region from a real Windows 10 install at 100% scaling.
3. Save it as `tests/pixdiff/references/<name>.png`.

When a pixdiff test disagrees with a number in `src/ui/theme.hpp`, **the number
is wrong.** Fix it there, not in the widget, so every consumer moves together.

## Fonts and icons — what cannot be shipped

Two things this project legally cannot bundle:

- **Segoe UI**, the Windows 10 shell font, is not redistributable. Install
  [Selawik](https://github.com/microsoft/Selawik), Microsoft's own MIT-licensed
  metric-compatible substitute, or supply Segoe UI from a machine you have
  licensed. The shell warns and keeps running on any other font, but metrics
  will not match and pixdiff tests will fail — correctly.
- **Icons** from `shell32.dll` / `imageres.dll` are copyrighted.

Reimplementing the look and behaviour is fine; redistributing Microsoft's
binary assets is not. See `docs/ASSETS.md`.

## Roadmap

1. ✅ X11 backend, strut-reserving bar, Blend2D surface
2. ✅ Widget/layout core, Windows-tuned text stack, pixel-diff harness
3. ⬜ Task buttons from real icons, grouping, hover thumbnails (XComposite), tray
4. ⬜ Context menus and the Start menu
5. ⬜ `finux-explorer`: nav pane, breadcrumb address bar, Details/Icons views
6. ⬜ File operations, ribbon, Trash, thumbnails
7. ⬜ Wayland backend (wlr-layer-shell + foreign-toplevel + screencopy)

**Wayland caveat:** the shell protocols exist only on wlroots compositors
(Sway, Hyprland, labwc, river) and KWin. GNOME/Mutter implements none of
layer-shell, foreign-toplevel-management or screencopy, so it cannot host this
shell at all. That is a protocol gap, not a porting task.

See `docs/DESIGN.md` for the architecture and `docs/ASSETS.md` for asset
sourcing.

## Licence

MIT. Not affiliated with or endorsed by Microsoft. "Windows", "Windows 10" and
"Segoe" are trademarks of Microsoft Corporation.

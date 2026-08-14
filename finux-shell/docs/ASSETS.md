# Assets

Reproducing an interface's *look and behaviour* is one thing; redistributing
the vendor's *binary assets* is another. This project does the first and not
the second. Nothing under version control here is Microsoft's.

That has practical consequences for anyone who wants pixel-exact output.

## Fonts

Windows 10 shell UI is **Segoe UI at 9pt** — 12px at 96 DPI. It is the single
largest contributor to "this looks like Windows," because text is on screen
everywhere at once.

Segoe UI is licensed with Windows and is not redistributable, so the shell
resolves a chain, in order:

| Family | Notes |
| --- | --- |
| `Segoe UI` | Exact. Only present if you installed it from a machine you have licensed. |
| `Selawik` | **The intended default.** Microsoft's own metric-compatible substitute, under the SIL Open Font License 1.1. Built for exactly this purpose, and — unlike Segoe UI — redistributable. |
| `DejaVu Sans` | Legibility backstop only. Metrically wrong; pixdiff tests are *expected* to fail against it. |

The shell prints a warning when it falls back past Selawik. That warning is not
noise — it means fidelity results are meaningless until resolved.

### Installing Selawik

```sh
tools/install-selawik.sh        # ~/.local/share/fonts
sudo tools/install-selawik.sh -s   # system-wide
```

Or by hand, from https://github.com/microsoft/Selawik/releases:

```sh
mkdir -p ~/.local/share/fonts/selawik
cp selawk*.ttf ~/.local/share/fonts/selawik/
fc-cache -f
fc-match "Selawik"   # must report Selawik, not a substitute
```

The archive carries five weights — Regular, Light, Semilight, Semibold and
Bold — matching the Segoe UI family the Windows shell draws from.

### Why the metrics matter

Selawik is not merely "a similar-looking font". Measured at the shell's 12px
UI size against the DejaVu Sans fallback:

| String | Selawik | DejaVu | Difference |
| --- | --- | --- | --- |
| `Type here to search` | 98px | 108px | −9.3% |
| `Untitled - Notepad` | 94px | 100px | −6.0% |
| `Productivity` | 57px | 66px | −13.6% |
| `4:44 PM` | 39px | 45px | −13.3% |

Vertical metrics happen to coincide at this size (ascent 12, descent 3, line
height 14), so the whole difference is horizontal — which is exactly what
drives label truncation, the measured clock width, and whether a task button's
title needs an ellipsis. Running against DejaVu does not make the shell look
*slightly* off; it makes every width-derived layout decision wrong.

`fc-match` matters: fontconfig always returns *something*, so a request for a
missing family silently yields DejaVu. `FontLibrary::resolveFamilyFile()`
compares the family fontconfig actually matched, which is what keeps the
fallback chain meaningful.

## Icons

The Windows icon set lives in `shell32.dll` and `imageres.dll` and is
copyrighted. Two supported paths:

1. **Draw an original set** in the Windows 10 flat-line style. This is the
   default and the only one that can be distributed.
2. **Extract from a licensed Windows install**, locally, opt-in:

   ```sh
   sudo apt install icoutils
   wrestool -x -t14 /path/to/imageres.dll -o icons/
   icotool -x icons/*.ico
   ```

   Never commit extracted resources. They are as redistributable as the DLL.

Real application icons on the taskbar do not need any of this — they come from
`_NET_WM_ICON` or the XDG icon theme via the `.desktop` entry matching
`WM_CLASS`, which is each application's own artwork.

## Reference screenshots

`tests/pixdiff/references/` is empty by design. Screenshots of the Windows UI
are Microsoft's, so the fidelity references are yours to capture:

1. Run the suite once. Each pixdiff test writes what it rendered to
   `tests/pixdiff/output/<name>.actual.png` and reports the missing reference.
2. On a real Windows 10 install at **100% scaling**, capture the same region at
   the same pixel dimensions. Lossless PNG; never a resized or JPEG-round-tripped
   capture, which would fail on compression artefacts rather than on geometry.
3. Save as `tests/pixdiff/references/<name>.png`.

Keep captures out of any published fork. They are useful to you locally and are
not yours to redistribute.

## Trademarks

"Windows", "Windows 10", "Segoe" and the Windows logo are trademarks of
Microsoft Corporation. This project is not affiliated with or endorsed by
Microsoft. The Start button currently draws a generic four-pane placeholder
rather than the Windows logo, and that is intentional.

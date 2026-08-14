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
| `Selawik` | **The intended default.** Microsoft's own MIT-licensed, metric-compatible substitute, built for exactly this purpose. |
| `DejaVu Sans` | Legibility backstop only. Metrically wrong; pixdiff tests are *expected* to fail against it. |

The shell prints a warning when it falls back past Selawik. That warning is not
noise — it means fidelity results are meaningless until resolved.

### Installing Selawik

```sh
# From https://github.com/microsoft/Selawik/releases
mkdir -p ~/.local/share/fonts
cp selawik*.ttf ~/.local/share/fonts/
fc-cache -f
fc-match "Selawik"   # must report Selawik, not a substitute
```

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

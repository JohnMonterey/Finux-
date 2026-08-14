#!/bin/sh
# Installs Selawik, the font the shell is designed around.
#
# Segoe UI ships with Windows and is not redistributable. Selawik is
# Microsoft's metric-compatible substitute under the SIL Open Font License, so
# it can be installed freely -- and metrics are not cosmetic here: Selawik runs
# 6-14% narrower than the DejaVu fallback at UI sizes, which shifts every
# label, every ellipsis cut and the width of the clock.
#
#   tools/install-selawik.sh            # current user
#   sudo tools/install-selawik.sh -s    # system-wide
set -eu

SYSTEM=0
[ "${1:-}" = "-s" ] && SYSTEM=1

if [ "$SYSTEM" -eq 1 ]; then
  DEST=/usr/share/fonts/truetype/selawik
else
  DEST="${XDG_DATA_HOME:-$HOME/.local/share}/fonts/selawik"
fi

RELEASE=https://github.com/microsoft/Selawik/releases/download/v1.01/selawik-1.01.zip
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "Fetching Selawik..."
if command -v curl >/dev/null 2>&1; then
  curl -fsSL "$RELEASE" -o "$WORK/selawik.zip"
elif command -v wget >/dev/null 2>&1; then
  wget -qO "$WORK/selawik.zip" "$RELEASE"
else
  echo "need curl or wget" >&2
  exit 1
fi

unzip -oq "$WORK/selawik.zip" -d "$WORK/extracted"
mkdir -p "$DEST"
find "$WORK/extracted" -iname '*.ttf' -exec cp {} "$DEST/" \;
cp "$WORK/extracted"/*.txt "$DEST/" 2>/dev/null || true

fc-cache -f "$DEST" >/dev/null 2>&1 || fc-cache -f >/dev/null 2>&1

# fontconfig always returns something, so a plain match proves nothing; check
# that the family it reports is actually Selawik.
if fc-match "Selawik" | grep -qi selawik; then
  echo "Installed to $DEST"
  fc-match "Selawik"
else
  echo "Installed to $DEST, but fontconfig still substitutes for Selawik." >&2
  echo "Try a fresh login, or run fc-cache -f as your own user." >&2
  exit 1
fi

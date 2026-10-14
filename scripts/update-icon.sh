#!/bin/sh
# Copies the designer's icon set into res/ (icon.ico is compiled into the exe, window, tray and installer).
set -e
SRC="${1:-/workspace/cast-recorder-icon}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
[ -f "$SRC/icon.ico" ] || { echo "missing $SRC/icon.ico" >&2; exit 1; }
cp "$SRC"/icon.ico "$DIR/res/icon.ico"
for f in "$SRC"/icon-*.png "$SRC"/icon.svg; do [ -f "$f" ] && cp "$f" "$DIR/res/"; done
echo "Icon updated from $SRC. Rebuild to embed it."

#!/bin/sh
# Draws hero.png and panels.png from source/ at twice their CSS size. Needs
# Google Chrome. The images in source/ are real captures from a run.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
chrome="${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

render() { # render <name> <width> <height>
  "$chrome" --headless=new --disable-gpu --hide-scrollbars \
    --user-data-dir="$work/profile" \
    --force-device-scale-factor=2 --window-size="$2,$3" \
    --virtual-time-budget=4000 \
    --screenshot="$here/$1.png" "file://$here/source/$1.html" >/dev/null 2>&1
  echo "wrote $here/$1.png"
}

render hero 1280 680
render panels 1280 320

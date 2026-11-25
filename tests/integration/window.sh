#!/usr/bin/env bash
# Runs the LVGL fixture with its window, which is a process of its own, and checks that the
# window outlives the app's restarts, download mode and a double reset, comes back when it's
# killed, and goes when the app quits.
# SDL's dummy driver runs the window's code with nothing on the screen, as CI has none. LVGL
# comes from the test suite's download, so build that first.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$root/tests/integration/app.sh"
export SDL_VIDEODRIVER=dummy

lvgl="$root/build/tests/_deps/lvgl-src"
[ -f "$lvgl/lvgl.h" ] || { echo "FAIL: no LVGL in $lvgl: build tests/runtime first" >&2; exit 1; }

scratch="$(mktemp -d)"
app_pid=""
window_socket=""
stop_all() {
  [ -z "$app_pid" ] || kill -9 "$app_pid" 2>/dev/null || true
  [ -z "$window_socket" ] || pkill -9 -f "preflash-window $window_socket" 2>/dev/null || true
  rm -rf "$scratch"
}
trap stop_all EXIT

fail() {
  echo "FAIL: $1" >&2
  [ -n "${2:-}" ] && printf '%s\n' "$2" >&2
  exit 1
}

# Kept between runs, so that only what changed gets rebuilt.
project="$root/build/integration/window/window_fixture"
mkdir -p "$project/managed_components"
rsync -a "$root/tests/fixtures/lvgl_project/" "$project/"
rsync -a --exclude /demos --exclude /docs --exclude /examples --exclude /tests \
  "$lvgl/" "$project/managed_components/lvgl__lvgl/"

go build -C "$root" -o "$scratch/preflash" ./cmd/preflash
pf() { "$scratch/preflash" "$@"; }

real_project="$(cd "$project" && pwd -P)"
sum="$(printf '%s' "$real_project" | shasum -a 256 | cut -c1-12)"
window_socket="$HOME/Library/Caches/preflash/$sum.window"

# This project's window processes, one pid a line.
windows() { pgrep -f "preflash-window $window_socket" || true; }

the_window() {
  local found
  found="$(windows)"
  [ -n "$found" ] || fail "no preflash-window process" "$(cat "$app_log")"
  [ "$(printf '%s\n' "$found" | wc -l | tr -d ' ')" -eq 1 ] || fail "more than one window process" "$found"
  printf '%s\n' "$found"
}

# Waits up to 2 s for the window to be gone.
expect_no_window() {
  local tenths
  for tenths in $(seq 1 20); do
    [ -z "$(windows)" ] && return 0
    sleep 0.1
  done
  fail "$1" "$(windows)"
}

# Size and whether anything is lit, from a PNG's header and its unfiltered rows: a filter
# can't make a black row anything but zeros.
png_pixels() {
  python3 -I - "$1" <<'PY'
import struct, sys, zlib, hashlib
data = open(sys.argv[1], "rb").read()
assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
width, height = struct.unpack(">II", data[16:24])
at, idat = 8, b""
while at < len(data):
    length, kind = struct.unpack(">I4s", data[at:at + 8])
    if kind == b"IDAT":
        idat += data[at + 8:at + 8 + length]
    at += 12 + length
raw = zlib.decompress(idat)
row = len(raw) // height
lit = any(b for r in range(height) for b in raw[r * row + 1:(r + 1) * row])
print(width, height, "lit" if lit else "black", hashlib.sha256(raw).hexdigest())
PY
}

# Clear of any window an earlier failure left behind, and built.
pkill -9 -f "preflash-window $window_socket" 2>/dev/null || true
pf run --project "$project" --headless <<< "@sim quit" || fail "the project didn't build and run"

# ---- What a headless run's socket sees, for the windowed one to match.
start_app "$scratch/preflash" run --project "$project" --headless
wait_for_output "ready, reset reason 1"
sleep 0.5
pf shot --project "$project" "$scratch/headless.png"
[ -z "$(windows)" ] || fail "a headless run started a window"
pf quit --project "$project"
finish_app
headless="$(png_pixels "$scratch/headless.png")"

# ---- window survives a restart
start_app "$scratch/preflash" run --project "$project"
wait_for_output "ready, reset reason 1"
first="$(the_window)"
pf send --project "$project" restart
wait_for_output "ready, reset reason 3"
sleep 0.5
[ "$(the_window)" = "$first" ] || fail "the restart replaced the window"

# ---- the socket still sees frames
pf shot --project "$project" "$scratch/windowed.png"
windowed="$(png_pixels "$scratch/windowed.png")"
case "$windowed" in
  "480 480 lit "*) ;;
  *) fail "the windowed shot isn't a lit 480x480 panel: $windowed" ;;
esac
[ "$windowed" = "$headless" ] || fail "the windowed shot differs from the headless one" "$windowed / $headless"

# ---- download mode blacks out the window's frame, so the next boot starts from black
frame_file="$HOME/.preflash/build/window_fixture-$(printf '%s' "$real_project" | shasum -a 256 | cut -c1-8)/panel.frame"
[ -f "$frame_file" ] || fail "no frame file at $frame_file"
# "lit" or "black", from the pixels after the frame file's 24-byte header.
frame_shows() {
  python3 -I - "$frame_file" <<'PY'
import struct, sys
data = open(sys.argv[1], "rb").read()
width, height = struct.unpack("<II", data[4:12])
print("lit" if any(data[24:24 + width * height * 2]) else "black")
PY
}
[ "$(frame_shows)" = lit ] || fail "the frame file isn't lit before download mode"
pf hardware --project "$project" button BOOT press > /dev/null
pf hardware --project "$project" button RST tap > /dev/null
wait_for_output "waiting for download"
blacked=""
for tenths in $(seq 1 20); do
  [ "$(frame_shows)" = black ] && { blacked=1; break; }
  sleep 0.1
done
[ -n "$blacked" ] || fail "download mode left the last frame in the frame file"
[ "$(the_window)" = "$first" ] || fail "download mode replaced the window"
pf hardware --project "$project" button BOOT release > /dev/null
pf hardware --project "$project" button RST tap > /dev/null
wait_for_output "ready, reset reason 1" 2

# ---- two resets 50 ms apart leave one window and one app, which answers
pf hardware --project "$project" button RST tap 10 > /dev/null &
first_tap=$!
sleep 0.05
pf hardware --project "$project" button RST tap 10 > /dev/null || fail "the second reset failed"
wait "$first_tap" || fail "the first reset failed"
wait_for_output "ready, reset reason 1" 3
sleep 1
[ "$(the_window)" = "$first" ] || fail "a double reset replaced the window"
kill -0 "$app_pid" 2>/dev/null || fail "the app ended in a double reset" "$(cat "$app_log")"
pid="$(pf status --project "$project" --json | python3 -I -c 'import json, sys; print(json.load(sys.stdin)["pid"])')" ||
  fail "the app stopped answering after a double reset"
[ "$pid" = "$app_pid" ] || fail "a double reset changed the app's pid: $pid, not $app_pid"

# ---- window killed
kill -9 "$first"
sleep 2
second="$(the_window)"
[ "$second" != "$first" ] || fail "the killed window is still there"
pf status --project "$project" > /dev/null || fail "the app stopped answering once its window was killed"
kill -0 "$app_pid" 2>/dev/null || fail "the app ended with its window" "$(cat "$app_log")"

# ---- quit closes it
pf quit --project "$project"
finish_app
expect_no_window "the window outlived a quit"

# ---- supervised quit leaves it
PREFLASH_SUPERVISED=1 start_app "$scratch/preflash" run --project "$project"
wait_for_output "ready, reset reason 1"
supervised="$(the_window)"
pf quit --project "$project"
finish_app
sleep 0.5
[ "$(the_window)" = "$supervised" ] || fail "a supervised quit closed the window"
printf '{"op":"close"}\n' | nc -U -w 2 "$window_socket" > /dev/null || true
expect_no_window "the window outlived its close"

echo "PASS: window"

#!/usr/bin/env bash
# Builds an unmodified LVGL project and checks the display from the outside: what
# it draws, what a touch does, and that headless runs never start SDL. LVGL comes
# from the test suite's download, so build that first.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export IDF_PATH="$root/runtime/idf"
export PREFLASH_BOARD="$root/tests/fixtures/board.json"
source "$root/tests/integration/app.sh"

lvgl="$root/build/tests/_deps/lvgl-src"
[ -f "$lvgl/lvgl.h" ] || { echo "FAIL: no LVGL in $lvgl: build tests/runtime first" >&2; exit 1; }

scratch="$(mktemp -d)"
app_pid=""
trap '[ -z "$app_pid" ] || kill -9 "$app_pid" 2>/dev/null; rm -rf "$scratch"' EXIT

fail() {
  echo "FAIL: $1" >&2
  [ -n "${2:-}" ] && printf '%s\n' "$2" >&2
  exit 1
}

# Where the fixture's button is, on a panel 320 by 480, and that same spot in the
# panel's coordinates once the firmware turns the display 90 degrees.
button_x=100
button_y=85
turned_x=$button_y
turned_y=$((480 - 1 - button_x))

check_shot() {
  python3 -I - "$1" <<'PY' || fail "bad screenshot" "$(cat "$app_log")"
import struct, sys
data = open(sys.argv[1], "rb").read()
assert data[:2] == b"BM", "not a BMP"
offset = struct.unpack_from("<I", data, 10)[0]
width, height = struct.unpack_from("<ii", data, 18)
assert (width, height) == (320, 480), f"size is {width}x{height}"
colours = {data[i:i + 3] for i in range(offset, len(data) - 2, 3)}
assert len(colours) > 1, "the panel is blank"
PY
}

project="$scratch/project"
cp -R "$root/tests/fixtures/lvgl_project" "$project"
mkdir "$project/managed_components"
rsync -a --exclude /demos --exclude /docs --exclude /examples --exclude /tests \
  "$lvgl/" "$project/managed_components/lvgl__lvgl/"
log="$(cmake -S "$project" -B "$scratch/build" 2>&1)" || fail "configure" "$log"
log="$(cmake --build "$scratch/build" --parallel 2>&1)" || fail "build" "$log"
app="$scratch/build/app"

# The driver doesn't exist, so a run that started SDL's video would fail. A windowed
# run has to fail here, or this check proves nothing.
export SDL_VIDEODRIVER=not_a_driver
unset PREFLASH_HEADLESS
start_app "$app"
wait_for_exit 50 || fail "a windowed run worked with no video driver" "$(cat "$app_log")"
finish_app
[ "$app_status" -ne 0 ] || fail "a windowed run exited cleanly with no video driver" "$(cat "$app_log")"
output="$(cat "$app_log")"
case "$output" in
  *"preflash: couldn't start SDL"*) ;;
  *) fail "no message about SDL" "$output" ;;
esac

export PREFLASH_HEADLESS=1

# A line sent before the firmware's console exists waits for it.
start_app "$app"
send "ping"
wait_for_output "ready, reset reason"
wait_for_output "pong"

send "@sim shot $scratch/shot.bmp"
wait_for_output "saved $scratch/shot.bmp"
check_shot "$scratch/shot.bmp"

send "@sim touch $button_x $button_y"
wait_for_output "clicked"

send "rotate 90"
wait_for_output "rotated 90"
send "@sim shot $scratch/turned.bmp"
wait_for_output "saved $scratch/turned.bmp"
check_shot "$scratch/turned.bmp"
cmp -s "$scratch/shot.bmp" "$scratch/turned.bmp" && fail "turning the display changed nothing"

send "@sim touch $turned_x $turned_y"
wait_for_output "clicked" 2
send "rotate 0"
wait_for_output "rotated 0"

# Sound is counted and never played, so a run with no output device doesn't hang.
send "@sim audio"
wait_for_output "audio samples written: 0"
send "play"
wait_for_output "played"
send "@sim audio"
wait_for_output "audio samples written: 3840"

send "@sim touch 9999 0"
wait_for_output "@sim touch needs x y"
send "@sim shot /nonexistent/folder/shot.bmp"
wait_for_output "@sim shot failed"
send "@sim nonesuch"
wait_for_output "Unknown control: nonesuch"

# A restart comes from a thread among many, and the firmware must come back up.
send "restart"
wait_for_output "ready, reset reason 3"
quit_app

# A firmware task that never lets go of the lock mustn't stop the process leaving.
start_app "$app"
wait_for_output "ready, reset reason"
send "hold"
wait_for_output "holding the lock"
kill -INT "$app_pid"
wait_for_exit 20 || fail "still running 2 s after SIGINT" "$(cat "$app_log")"
finish_app
[ "$app_status" -eq 0 ] || fail "SIGINT exited with status $app_status" "$(cat "$app_log")"

echo "PASS: display"

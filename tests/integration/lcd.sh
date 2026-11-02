#!/usr/bin/env bash
# Boots a project with no board package on the 3.49 V2 board and drives it from outside: the
# panel through esp_lcd alone, its big-endian pixels, the backlight on LEDC and the expander,
# the two I2C buses, touch, and a USB serial driver of the firmware's own. LVGL comes from
# the test suite's download, so build that first.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
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

# Kept between runs, so that only what changed gets rebuilt.
project="$root/build/integration/lcd/lcd_fixture"
mkdir -p "$project/managed_components"
rsync -a "$root/tests/fixtures/lcd_project/" "$project/"
rsync -a --exclude /demos --exclude /docs --exclude /examples --exclude /tests \
  "$lvgl/" "$project/managed_components/lvgl__lvgl/"

go build -C "$root" -o "$scratch/preflash" ./cmd/preflash
pf() { "$scratch/preflash" "$@"; }
board="ESP32-S3-Touch-LCD-3.49-V2"

real_project="$(cd "$project" && pwd -P)"
sum="$(printf '%s' "$real_project" | shasum -a 256 | cut -c1-12)"
port="$HOME/Library/Caches/preflash/$sum.tty"

# Each argument is x,y,r,g,b for a pixel of a shot of the 172 by 640 panel. Prints what it
# found if any is wrong.
shot_shows() {
  local path="$scratch/shot.bmp"
  pf shot --project "$project" "$path"
  python3 -I - "$path" "${@:1}" <<'PY'
import struct, sys
data = open(sys.argv[1], "rb").read()
assert data[:2] == b"BM", "not a BMP"
offset = struct.unpack_from("<I", data, 10)[0]
width, height = struct.unpack_from("<ii", data, 18)
assert (width, height) == (172, 640), f"size is {width}x{height}"
stride = (width * 3 + 3) & ~3
for want in sys.argv[2:]:
    x, y, r, g, b = map(int, want.split(","))
    at = offset + (height - 1 - y) * stride + x * 3
    found = (data[at + 2], data[at + 1], data[at])
    assert found == (r, g, b), f"at {x},{y}: {found}, wanted {(r, g, b)}"
PY
}

# LVGL and the host draw on their own time, so this asks again until the panel shows it.
wait_for_shot() {
  local tries
  for tries in $(seq 1 20); do
    shot_shows "$@" 2>/dev/null && return 0
    sleep 0.25
  done
  shot_shows "$@" || fail "the panel never showed: $*" "$(cat "$app_log")"
}

red=255,0,0
green=0,255,0
blue=0,0,255
white=255,255,255
black=0,0,0

# The first build can take longer than a test's wait for output.
pf run --project "$project" --board "$board" --headless <<< "@sim quit" \
  || fail "the project didn't build and run"

start_app "$scratch/preflash" run --project "$project" --board "$board" --headless
wait_for_output "ready"

pf status --json --project "$project" | python3 -I -c '
import json, sys
s = json.load(sys.stdin)
assert s["board"] == "ESP32-S3-Touch-LCD-3.49-V2", s
assert (s["width"], s["height"]) == (172, 640), s
' || fail "status doesn't show the board"

# Big-endian pixels come out as the colours firmware meant, the right way up.
send "pattern"
wait_for_output "pattern drawn in 10 chunks"
wait_for_shot "10,10,$red" "160,10,$green" "160,630,$blue" "10,630,$white"

# LVGL on the panel, through the firmware's own flush, and its rotation. LVGL's quarter turn
# takes its top left to the panel's bottom left.
send "redraw"
wait_for_output "redrawn"
wait_for_shot "10,10,$red" "160,10,$black" "160,630,$black" "10,630,$black"
send "rotate 1"
wait_for_output "rotated 1"
wait_for_shot "10,630,$red" "10,10,$black" "160,10,$black" "160,630,$black"
send "rotate 2"
wait_for_output "rotated 2"
wait_for_shot "160,630,$red" "10,630,$black" "10,10,$black" "160,10,$black"
send "rotate 3"
wait_for_output "rotated 3"
wait_for_shot "160,10,$red" "160,630,$black" "10,630,$black" "10,10,$black"
send "rotate 0"
wait_for_output "rotated 0"

# LVGL's lock works, so firmware can share it with LVGL's task.
send "lock"
wait_for_output "flushes during the lock 0, after it 1"

# The enable bit on the expander starts low and cuts the backlight whatever the PWM says.
send "@sim brightness"
wait_for_output "brightness: 0 (pwm 100, enable exio1 low)"
send "backlight 60"
wait_for_output "backlight 60"
send "@sim brightness"
wait_for_output "brightness: 60 (pwm 60, enable exio1 high)"
send "backlight 0"
wait_for_output "backlight 0"
send "@sim brightness"
wait_for_output "brightness: 0 (pwm 0, enable exio1 low)"

# The pull-up holds the active-low key high.
send "key"
wait_for_output "key level 1"

# A chip answers on its own bus only.
send "buses"
wait_for_output "touch on bus 0: ESP_ERR_INVALID_STATE"
wait_for_output "expander on bus 1: ESP_ERR_INVALID_STATE"
wait_for_output "expander on bus 0: ESP_OK"

send "motion"
wait_for_output "accel 0 16384 0"

# Touch comes through the chip on bus 1, in the panel's coordinates.
pf touch --project "$project" 40 600
wait_for_output "touch 40 600"
wait_for_output "clicked"

# The port takes bytes to the firmware's driver, and a companion's @ lines are its own.
python3 -I - "$port" <<'PY' || fail "the companion" "$(cat "$app_log")"
import os, select, sys, termios, time

fd = os.open(sys.argv[1], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
attrs = termios.tcgetattr(fd)
attrs[0] = attrs[1] = attrs[3] = 0
attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
attrs[4] = attrs[5] = termios.B115200
termios.tcsetattr(fd, termios.TCSANOW, attrs)

def wait_for(text):
    heard = b""
    deadline = time.time() + 10
    while text not in heard:
        if time.time() > deadline:
            sys.exit(f"never heard {text!r}: {heard[-300:]!r}")
        if select.select([fd], [], [], 0.1)[0]:
            heard += os.read(fd, 65536)

os.write(fd, b"@T\t1793455320\t0\n")
wait_for(b"protocol line: @T\t1793455320\t0")
os.write(fd, b"@sim quit\n")
wait_for(b"protocol line: @sim quit")
# A line can arrive in pieces, and keep its carriage return for the firmware to take off.
os.write(fd, b"re")
time.sleep(0.2)
os.write(fd, b"ason\r\n")
wait_for(b"reset reason 1")
os.close(fd)
PY

# A line sent to a restart waits for the next boot's driver.
send "restart"
wait_for_output "ready" 2
send "reason"
wait_for_output "reset reason 3"
send "backlight 60"
wait_for_output "backlight 60" 2
send "@sim brightness"
wait_for_output "brightness: 60 (pwm 60, enable exio1 high)" 2

# A line that isn't a command is the firmware's to answer.
send "nothing"
wait_for_output "unknown command"
quit_app

echo "PASS: lcd"

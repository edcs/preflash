#!/usr/bin/env bash
# The main end-to-end test. It runs examples/demo as a clean clone would, on both stock boards,
# with no ESP-IDF and no network of its own: LVGL comes from the lock file, pinned by hash, and
# the internet stays forced offline so that no frame depends on a public site.
#
# Every page, shortcut, forced failure and crash is driven through the control socket, and the
# panel is compared with tests/integration/golden/demo/ pixel by pixel. Frames match exactly
# because the firmware renders in software from a pinned LVGL and the demo's freeze command
# stops the clock and the ring. A frame still changing shows up as a failure here.
#
# After a deliberate change to how the demo looks, regenerate the frames and read the diff:
#
#   UPDATE_GOLDEN=1 tests/integration/demo.sh
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$root/tests/integration/app.sh"

scratch="$(mktemp -d)"
app_pid=""
trap '[ -z "$app_pid" ] || kill -9 "$app_pid" 2>/dev/null || true; rm -rf "$scratch"' EXIT

fail() {
  echo "FAIL: $1" >&2
  [ -n "${2:-}" ] && printf '%s\n' "$2" >&2
  exit 1
}

demo="$root/examples/demo"
held_at="2026-07-14T09:41:00Z"
rejects="$root/build/integration/demo-rejects"
rm -rf "$rejects"

# The firmware stays an ordinary ESP-IDF project.
firmware=(--exclude=sdkconfig --exclude='sdkconfig.*' --exclude=dependencies.lock --exclude=README.md --exclude=.preflashrc)
matches="$(grep -rilE 'snoozel|nyx' "${firmware[@]}" "$demo" || true)"
[ -z "$matches" ] || fail "the demo mentions a private project" "$matches"
matches="$(grep -rilE --include='*.cpp' --include='*.h' --include=CMakeLists.txt 'preflash|simulator' "$demo" || true)"
[ -z "$matches" ] || fail "the firmware knows about the tool" "$matches"
matches="$(grep -rnE '^\s*#\s*(ifdef|ifndef|if)\b' "$demo/main" || true)"
[ -z "$matches" ] || fail "the firmware has conditional compilation" "$matches"
[ ! -e "$demo/managed_components" ] || fail "the checkout has managed_components, which a clean clone wouldn't"

go build -C "$root" -o "$scratch/preflash" ./cmd/preflash
pf() { "$scratch/preflash" "$@"; }

# Kept between runs, so that only what changed gets rebuilt. One folder per board, because a
# board's sdkconfig rebuilds everything.
use_project() {
  project="$root/build/integration/demo/$1/demo"
  mkdir -p "$project"
  rsync -a --delete "$demo/" "$project/"
  real_project="$(cd "$project" && pwd -P)"
  sum="$(printf '%s' "$real_project" | shasum -a 256 | cut -c1-8)"
  rm -rf "$HOME/.preflash/state/demo-$sum"
  rm -f "$HOME/.preflash/build/demo-$sum/crash.json"
  golden="$root/tests/integration/golden/demo/$1"
  mkdir -p "$golden"
}

snapshot() { (cd "$project" && find . -type f -exec shasum -a 256 {} + | sort -k2); }

# Serial output is checked from a mark onwards, so that an old line can't pass for a new one.
mark() { marked="$(wc -l < "$app_log")"; }

saw() {
  local tenths
  for tenths in $(seq 1 100); do
    tail -n "+$((marked + 1))" "$app_log" | grep -qF -- "$1" && return 0
    kill -0 "$app_pid" 2>/dev/null || fail "the app stopped before printing: $1" "$(cat "$app_log")"
    sleep 0.1
  done
  fail "timed out waiting for: $1" "$(tail -n 40 "$app_log")"
}

# Prints nothing if the two PNGs show the same pixels, and how many differ if not.
compare_frames() {
  python3 -I - "$1" "$2" <<'PY'
import struct, sys, zlib

def pixels(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    at, idat = 8, b""
    while at < len(data):
        length, kind = struct.unpack_from(">I4s", data, at)
        body = data[at + 8:at + 8 + length]
        at += 12 + length
        if kind == b"IHDR":
            width, height, depth, colour = struct.unpack_from(">IIBB", body)
            assert depth == 8 and colour in (2, 6), f"colour type {colour}"
        elif kind == b"IDAT":
            idat += body
    channels = 3 if colour == 2 else 4
    stride = width * channels
    raw = zlib.decompress(idat)
    previous = bytearray(stride)
    rows = []
    for y in range(height):
        start = y * (stride + 1)
        kind, line = raw[start], bytearray(raw[start + 1:start + 1 + stride])
        for x in range(stride):
            left = line[x - channels] if x >= channels else 0
            up = previous[x]
            corner = previous[x - channels] if x >= channels else 0
            if kind == 1:
                line[x] = (line[x] + left) & 255
            elif kind == 2:
                line[x] = (line[x] + up) & 255
            elif kind == 3:
                line[x] = (line[x] + (left + up) // 2) & 255
            elif kind == 4:
                p = left + up - corner
                near = min((abs(p - left), 0, left), (abs(p - up), 1, up), (abs(p - corner), 2, corner))
                line[x] = (line[x] + near[2]) & 255
        rows.append(bytes(line))
        previous = line
    return width, height, channels, rows

a, b = pixels(sys.argv[1]), pixels(sys.argv[2])
if a[:2] != b[:2]:
    print(f"size {a[:2]} against {b[:2]}")
elif a != b:
    wide = a[2]
    different = sum(
        ra[x:x + wide] != rb[x:x + wide]
        for ra, rb in zip(a[3], b[3])
        for x in range(0, len(ra), wide)
    )
    print(f"{different} pixels differ")
PY
}

# The panel settles on its own time, so this looks again until it matches.
frame() {
  local name="$1" shot="$scratch/shot.png" want="$golden/$1.png" tries difference
  if [ -n "${UPDATE_GOLDEN:-}" ]; then
    # Longer than the firmware's poll of its chips, so that a change on its way isn't missed.
    local previous="$scratch/previous.png" steady=0
    pf shot --project "$project" "$previous"
    for tries in $(seq 1 40); do
      sleep 0.4
      pf shot --project "$project" "$shot"
      if [ -z "$(compare_frames "$previous" "$shot")" ]; then
        steady=$((steady + 1))
      else
        steady=0
      fi
      if [ "$steady" -ge 4 ]; then
        cp "$shot" "$want"
        return 0
      fi
      cp "$shot" "$previous"
    done
    fail "$name never held still"
  fi
  [ -f "$want" ] || fail "no golden frame $want: see the top of this script"
  for tries in $(seq 1 24); do
    pf shot --project "$project" "$shot"
    difference="$(compare_frames "$shot" "$want")"
    [ -n "$difference" ] || return 0
    sleep 0.25
  done
  mkdir -p "$rejects"
  cp "$shot" "$rejects/$(basename "$golden")-$name.png"
  fail "$name doesn't match its golden frame: $difference, kept in $rejects"
}

expect_in() {
  case "$2" in
    *"$1"*) ;;
    *) fail "lacks: $1" "$2" ;;
  esac
}

# What the console's status command says, once it has said it.
console_status() {
  mark
  pf send --project "$project" status
  saw "sound="
  tail -n "+$((marked + 1))" "$app_log"
}

hold_clock() {
  mark
  pf send --project "$project" freeze "$held_at"
  saw "clock=held"
}

boot() {
  start_app "$scratch/preflash" run --project "$project" --headless "$@"
  # A first run includes the build.
  local tenths
  for tenths in $(seq 1 6000); do
    grep -qF "ready on" "$app_log" && break
    kill -0 "$app_pid" 2>/dev/null || fail "the app didn't start" "$(cat "$app_log")"
    sleep 0.1
  done
  grep -qF "ready on" "$app_log" || fail "the app never came up" "$(cat "$app_log")"
  mark
}

restart() {
  mark
  pf send --project "$project" restart
  saw "ready on"
  mark
}

quit() {
  pf quit --project "$project"
  finish_app
  [ "$app_status" -eq 0 ] || fail "the app quit with status $app_status" "$(cat "$app_log")"
}

# Force a failure and see it on the faults page, then put it back.
forced() {
  local name="$1" setting="$2" healthy="$3" expected="$4"
  pf hardware --project "$project" "$name" "$setting" > /dev/null
  frame "faults-$name-$setting"
  expect_in "$expected" "$(console_status)"
  pf hardware --project "$project" "$name" "$healthy" > /dev/null
}

# Touches open a page from the home page and come back, with the frame in between.
visit() {
  local name="$1" x="$2" y="$3"
  mark
  pf touch --project "$project" "$x" "$y"
  saw "screen=$name"
  frame "$name"
}

go_back() {
  mark
  pf touch --project "$project" "$back" "$back"
  saw "screen=home"
}

# Runs each shortcut's lines through the console, with the values a person would be offered.
send_shortcuts() {
  local today line
  today="$(date +%F)"
  awk '/^shortcuts:/ { on = 1; next } /^[a-z]/ { on = 0 }
       on && /send:/ { sub(/.*send: */, ""); if ($0 != "") print; next }
       on && /^ +- [a-z]/ && !/label:/ { sub(/^ +- */, ""); print }' "$project/.preflashrc" |
    while IFS= read -r line; do
      line="${line//\{today\}/$today}"
      line="${line//\{percent\}/60}"
      echo "$line"
    done > "$scratch/shortcuts"
  [ "$(wc -l < "$scratch/shortcuts")" -ge 7 ] || fail "the shortcuts didn't parse" "$(cat "$scratch/shortcuts")"
  while IFS= read -r line; do
    pf send --project "$project" "$line"
  done < "$scratch/shortcuts"
}

crash_with() {
  local line="$1" expected_title="$2" expected_place="$3" report
  boot "${run_flags[@]}"
  pf send --project "$project" "$line"
  wait_for_exit 200 || fail "$line left the app running" "$(cat "$app_log")"
  finish_app
  [ "$app_status" -ne 0 ] || fail "$line exited cleanly" "$(cat "$app_log")"
  report="$(pf crash --project "$project")"
  expect_in "$expected_title" "$report"
  expect_in "$expected_place" "$report"
}

common_failures() {
  forced i2c pcf85063 healthy "rtc=no answer"
  forced i2c qmi8658 healthy "motion=no answer"
  pf hardware --project "$project" wifi_join fails > /dev/null
  mark
  pf send --project "$project" wifi another-network
  saw "gave up joining"
  frame "faults-wifi-fails"
  pf hardware --project "$project" wifi_join succeeds > /dev/null
  mark
  pf send --project "$project" wifi demo-network
  saw "network: joined"
  mark
  pf send --project "$project" fetch
  saw "network: fetch: "
}

next_boot_failures() {
  pf hardware --project "$project" rtc flat > /dev/null
  restart
  pf send --project "$project" page time
  frame "time-flat"
  pf send --project "$project" page faults
  frame "faults-rtc-flat"
  pf hardware --project "$project" rtc healthy > /dev/null

  mark
  pf send --project "$project" time "$held_at"
  saw "time set"
  pf hardware --project "$project" motion gone > /dev/null
  restart
  pf send --project "$project" page motion
  frame "motion-gone"
  pf hardware --project "$project" motion healthy > /dev/null
  restart
}

# Saved settings survive a restart. The trace shows them going through NVS.
settings_survive() {
  local record status
  mark
  pf send --project "$project" brightness 30
  saw "brightness=30"
  record="$(pf trace --project "$project" --stream nvs --save "$scratch/trace.jsonl" 2>&1 || true)"
  restart
  expect_in "brightness=30" "$(console_status)"
  status="$(pf status --project "$project" --json)"
  expect_in '"backlight":30' "$status"
  pf send --project "$project" brightness 80
}

run_flags=()

# ---- ESP32-S3-Touch-LCD-4B, through the board package and the project's own board file

use_project 4b
back=38
before="$(snapshot)"
boot
expect_in "demo-4b" "$(pf status --project "$project")"
expect_in "version=1.0.0" "$(console_status)"
pf hardware --project "$project" internet offline > /dev/null
restart
hold_clock
frame home

visit time 69 288; go_back
visit motion 183 288; go_back
visit power 297 288; go_back
visit sound 411 288
mark
pf touch --project "$project" 240 432
send "@sim audio"
wait_for_output "audio samples written"
frame sound-played
go_back
visit network 69 410
mark
pf touch --project "$project" 240 432
saw "network: fetch: "
frame network-fetched
go_back
visit update 183 410
mark
pf touch --project "$project" 240 432
saw "update: check failed"
frame update-checked
go_back
visit settings 297 410
mark
pf touch --project "$project" 240 121
frame settings-moved
expect_in "brightness=54" "$(console_status)"
go_back
pf send --project "$project" brightness 80
visit faults 411 410

forced i2c axp2101 healthy "power=no answer"
forced i2c all healthy "rtc=no answer"
common_failures

pf send --project "$project" page power
pf hardware --project "$project" battery 15 > /dev/null
frame power-battery-15
pf hardware --project "$project" usb unplugged > /dev/null
frame power-unplugged
pf hardware --project "$project" usb plugged > /dev/null
pf hardware --project "$project" battery none > /dev/null

pf send --project "$project" page home
for degrees in 90 180 270; do
  pf hardware --project "$project" rotation "$degrees" > /dev/null
  frame "home-turned-$degrees"
done
pf hardware --project "$project" rotation 0 > /dev/null
frame home-after-turning

next_boot_failures
settings_survive

hold_clock
send_shortcuts
saw "brightness=60"
saw "clock=held"
hold_clock
pf send --project "$project" page home
pf send --project "$project" brightness 80
quit
[ "$(snapshot)" = "$before" ] || fail "running changed the project's folder"

crash_with "crash null" "Null pointer read in" "mishaps.cpp"
crash_with "crash abort" "abort" "mishaps.cpp"
crash_with "crash check" "ESP_ERR_INVALID_STATE" "mishaps.cpp"
crash_with "hang" "watchdog" "monitor"

# Turning off through the power chip ends the app.
boot
pf send --project "$project" poweroff
wait_for_exit 100 || fail "poweroff left the app running" "$(cat "$app_log")"
finish_app

# ---- ESP32-S3-Touch-LCD-3.49-V2, through esp_lcd, with a build of its own

use_project 349
back=30
run_flags=(--board ESP32-S3-Touch-LCD-3.49-V2)
boot "${run_flags[@]}"
expect_in "ESP32-S3-Touch-LCD-3.49-V2" "$(pf status --project "$project")"
expect_in "sound=no" "$(console_status)"
pf hardware --project "$project" internet offline > /dev/null
restart
hold_clock
frame home

visit time 45 275; go_back
visit motion 127 275; go_back
visit power 45 379; go_back
visit sound 127 379; go_back
visit network 45 483
mark
pf touch --project "$project" 86 608
saw "network: fetch: "
frame network-fetched
go_back
visit update 127 483
mark
pf touch --project "$project" 86 608
saw "update: check failed"
frame update-checked
go_back
visit settings 45 587; go_back
visit faults 127 587

common_failures
pf send --project "$project" page home
pf hardware --project "$project" rotation 180 > /dev/null
frame home-turned-180
# The long panel has no layout for sideways, so the screen stays where it is.
pf hardware --project "$project" rotation 90 > /dev/null
frame home-turned-180-stays
pf hardware --project "$project" rotation 0 > /dev/null
frame home-after-turning

next_boot_failures
settings_survive
quit

crash_with "crash null" "Null pointer read in" "mishaps.cpp"

# ---- The benchmark, which prints both of its forms

use_project 4b
out="$(pf bench --project "$project" 2> "$scratch/table")" || fail "bench failed" "$(cat "$scratch/table")"
expect_in "frames_per_second" "$out"
expect_in "Serial throughput" "$(cat "$scratch/table")"

echo "demo.sh passed"

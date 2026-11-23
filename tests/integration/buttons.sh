#!/usr/bin/env bash
# Presses the board's buttons through the command line, headless. RST resets the chip as EN
# does: the firmware boots again with NVS kept and reads a power-on reset. BOOT held through
# RST enters download mode, and two resets close together leave one app.
#
# The demo shows the boots and NVS. It doesn't print its reset reason, so the LVGL fixture,
# which does, shows that RST reads as a power-on reset where a restart reads as a software
# one. LVGL comes from the test suite's download, so build that first.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$root/tests/integration/app.sh"

lvgl="$root/build/tests/_deps/lvgl-src"
[ -f "$lvgl/lvgl.h" ] || { echo "FAIL: no LVGL in $lvgl: build tests/runtime first" >&2; exit 1; }

scratch="$(mktemp -d)"
app_pid=""
trap '[ -z "$app_pid" ] || kill -9 "$app_pid" 2>/dev/null || true; rm -rf "$scratch"' EXIT

fail() {
  echo "FAIL: $1" >&2
  [ -n "${2:-}" ] && printf '%s\n' "$2" >&2
  exit 1
}

expect_in() {
  case "$2" in
    *"$1"*) ;;
    *) fail "lacks: $1" "$2" ;;
  esac
}

go build -C "$root" -o "$scratch/preflash" ./cmd/preflash
pf() { "$scratch/preflash" "$@"; }

# Serial output is checked from a mark onwards, so that an old line can't pass for a new one.
mark() { marked="$(wc -l < "$app_log")"; }

saw() {
  local tenths
  for tenths in $(seq 1 $(( ${PREFLASH_TEST_WAIT:-10} * 10 ))); do
    tail -n "+$((marked + 1))" "$app_log" | grep -qF -- "$1" && return 0
    kill -0 "$app_pid" 2>/dev/null || fail "the app stopped before printing: $1" "$(cat "$app_log")"
    sleep 0.1
  done
  fail "timed out waiting for: $1" "$(tail -n 40 "$app_log")"
}

# Starts the app and waits for a line, which the first run reaches only after a build.
boot_until() {
  local text="$1" tenths
  shift
  start_app "$scratch/preflash" run --project "$project" --headless "$@"
  for tenths in $(seq 1 6000); do
    grep -qF -- "$text" "$app_log" && break
    kill -0 "$app_pid" 2>/dev/null || fail "the app didn't start" "$(cat "$app_log")"
    sleep 0.1
  done
  grep -qF -- "$text" "$app_log" || fail "the app never came up" "$(cat "$app_log")"
  mark
}

quit() {
  pf quit --project "$project"
  finish_app
  [ "$app_status" -eq 0 ] || fail "the app quit with status $app_status" "$(cat "$app_log")"
}

status_json() { pf status --project "$project" --json; }

# One field of status, as JSON writes it.
field() { status_json | python3 -I -c 'import json, sys; print(json.dumps(json.load(sys.stdin).get(sys.argv[1])))' "$1"; }

button() { pf hardware --project "$project" button "$@"; }

# Waits for the demo's console to answer. A line sent while the app restarts is lost with
# the old boot, so this asks again.
firmware_answers() {
  local tries tenths
  for tries in $(seq 1 10); do
    mark
    pf send --project "$project" status
    for tenths in $(seq 1 20); do
      tail -n "+$((marked + 1))" "$app_log" | grep -qF "sound=" && return 0
      sleep 0.1
    done
  done
  fail "the firmware never answered" "$(tail -n 40 "$app_log")"
}

# ---- RST reads as a power-on reset, on the LVGL fixture

project="$root/build/integration/buttons/fixture"
mkdir -p "$project/managed_components"
rsync -a "$root/tests/fixtures/lvgl_project/" "$project/"
rsync -a --exclude /demos --exclude /docs --exclude /examples --exclude /tests \
  "$lvgl/" "$project/managed_components/lvgl__lvgl/"

boot_until "ready, reset reason 1"
pf send --project "$project" restart
saw "ready, reset reason 3"
mark
button RST tap
saw "ready, reset reason 1"
quit

# ---- The demo

project="$root/build/integration/buttons/demo"
mkdir -p "$project"
rsync -a --delete "$root/examples/demo/" "$project/"
real_project="$(cd "$project" && pwd -P)"
sum="$(printf '%s' "$real_project" | shasum -a 256 | cut -c1-8)"
rm -rf "$HOME/.preflash/state/demo-$sum"

boot_until "ready on"
grep -qF "settings: boot 1," "$app_log" || fail "the first boot isn't boot 1" "$(cat "$app_log")"
pid="$(field pid)"
[ "$pid" = "$app_pid" ] || fail "status names pid $pid, not the app's $app_pid"
[ "$(field power)" = '"on"' ] || fail "a fresh run isn't on" "$(status_json)"

# RST boots the firmware again, with what it saved in NVS.
pf send --project "$project" brightness 30
saw "brightness=30"
mark
button RST tap
saw "settings: boot 2, brightness 30"
saw "ready on"
[ "$(field pid)" = "$pid" ] || fail "RST changed the app's pid"

# BOOT held through RST enters download mode, which runs no firmware and has no serial port.
button BOOT press
mark
button RST tap
saw "waiting for download"
[ "$(field power)" = '"download"' ] || fail "status doesn't say download mode" "$(status_json)"
[ "$(field serial_port)" = '""' ] || fail "download mode kept the serial port" "$(status_json)"
expect_in "power      download" "$(pf status --project "$project")"
button BOOT release
sleep 1
if tail -n "+$((marked + 1))" "$app_log" | grep -qF "ready on"; then
  fail "the firmware ran in download mode" "$(tail -n 40 "$app_log")"
fi

# The next RST with BOOT up boots normally.
mark
button RST tap
saw "settings: boot 3,"
saw "ready on"
[ "$(field power)" = '"on"' ] || fail "RST didn't leave download mode" "$(status_json)"
[ "$(field serial_port)" != '""' ] || fail "the serial port didn't come back" "$(status_json)"

# Two resets 50 ms apart leave one app, which answers.
mark
button RST tap 10 &
first=$!
sleep 0.05
button RST tap 10 || fail "the second reset failed"
wait "$first" || fail "the first reset failed"
saw "ready on"
firmware_answers
[ "$(field pid)" = "$pid" ] || fail "a double reset changed the app's pid"
app="$(ps -o command= -p "$pid")"
running="$(pgrep -fx -- "$app" | wc -l | tr -d ' ')"
[ "$running" = 1 ] || fail "a double reset left $running apps running" "$(pgrep -fl -- "$app")"
[ "$(field power)" = '"on"' ] || fail "the double reset didn't end on" "$(status_json)"
quit

echo "buttons.sh passed"

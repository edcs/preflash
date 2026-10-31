#!/usr/bin/env bash
# Runs a real product, Snoozel, on its own drivers from a clean copy of its main
# branch: nothing here may change the firmware or write inside the copy. Point
# SNOOZEL at a checkout that has run idf.py once, for sdkconfig and
# managed_components.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
snoozel="${SNOOZEL:-$root/../snoozel}"
mod_board="$root/tests/fixtures/snoozel-4b.yaml"
source "$root/tests/integration/app.sh"

scratch="$(mktemp -d)"
app_pid=""
state=""
trap '[ -z "$app_pid" ] || kill -9 "$app_pid" 2>/dev/null; rm -rf "$scratch"; [ -z "$state" ] || rm -rf "$state"' EXIT

fail() {
  echo "FAIL: $1" >&2
  [ -n "${2:-}" ] && printf '%s\n' "$2" >&2
  exit 1
}

# Kept between runs, so that only what changed gets rebuilt.
copy="$root/build/integration/snoozel"
if [ -d "$copy/.git" ]; then
  git -C "$copy" fetch --quiet "$snoozel" main
  git -C "$copy" reset --quiet --hard FETCH_HEAD
else
  mkdir -p "$(dirname "$copy")"
  git clone --quiet --branch main "$snoozel" "$copy"
fi
rsync -a "$snoozel/sdkconfig" "$copy/sdkconfig"
rsync -a --delete "$snoozel/managed_components/" "$copy/managed_components/"
[ -z "$(git -C "$copy" status --porcelain)" ] || fail "the copy of the project isn't clean"

matches="$(grep -ril snoozel "$root/runtime" "$root/boards" || true)"
[ -z "$matches" ] || fail "the tool mentions the project" "$matches"

go build -C "$root" -o "$scratch/preflash" ./cmd/preflash

before="$(git -C "$copy" status --porcelain --ignored)"

# The first build can take longer than a test's wait for output.
"$scratch/preflash" run --project "$copy" --board "$mod_board" --headless <<< "@sim quit" \
  || fail "the project didn't build and run"

# Each run below starts from no saved state, in the folder preflash uses for this copy.
real_copy="$(cd "$copy" && pwd -P)"
state="$HOME/.preflash/state/snoozel-$(printf '%s' "$real_copy" | shasum -a 256 | cut -c1-8)"
[ -d "$state" ] || fail "no saved state in $state"
rm -rf "$state"

# The newest N in the log's lines holding this text.
last_number() {
  grep -F -- "$1" "$app_log" | tail -n 1 | sed 's/[^0-9]//g'
}

# Whether the BMP is all black, or holds a picture of more than one colour. That tells
# the frames apart without pinning down the artwork.
shot_is() {
  python3 -I - "$1" "$2" <<'PY'
import struct, sys
data = open(sys.argv[1], "rb").read()
assert data[:2] == b"BM", "not a BMP"
offset = struct.unpack_from("<I", data, 10)[0]
width, height = struct.unpack_from("<ii", data, 18)
assert (width, height) == (480, 480), f"size is {width}x{height}"
pixels = data[offset:]
if sys.argv[2] == "black":
    assert not any(pixels), "not all black"
else:
    assert len({pixels[i:i + 3] for i in range(0, len(pixels) - 2, 3)}) > 1, "one colour"
PY
}

# LVGL draws on its own time, so this asks again until the panel shows it.
wait_for_shot() {
  local want="$1" tries path
  for tries in $(seq 1 20); do
    path="$scratch/$want-$tries.bmp"
    send "@sim shot $path"
    wait_for_output "saved $path"
    shot_is "$path" "$want" 2>/dev/null && return 0
    sleep 0.5
  done
  fail "the panel never showed $want" "$(cat "$app_log")"
}

# The firmware sets the backlight on its own clock, so this asks again as well.
wait_for_brightness() {
  local tries
  for tries in $(seq 1 20); do
    send "@sim brightness"
    sleep 0.25
    [ "$(grep -F 'brightness: ' "$app_log" | tail -n 1)" = "$1" ] && return 0
  done
  fail "the backlight never reached: $1" "$(cat "$app_log")"
}

# Pressing again, because a press in the same tick as the clock's jump counts for the
# old phase. A night check is lit too, so this waits for the day's full brightness.
tap_until_bright() {
  local tries
  for tries in $(seq 1 20); do
    send "tap"
    sleep 0.25
    send "@sim brightness"
    sleep 0.25
    [ "$(grep -F 'brightness: ' "$app_log" | tail -n 1)" = "$1" ] && return 0
  done
  fail "a tap never lit the day: $1" "$(cat "$app_log")"
}

start_run() {
  start_app "$scratch/preflash" run --project "$copy" "$@" --headless
}

# Boots with the RTC at a chosen time, so that no run depends on the hour of the test.
start_run_at() {
  local time="$1"
  shift
  export PREFLASH_RTC_START="$time"
  start_run "$@"
  unset PREFLASH_RTC_START
}

noon="2026-11-01T12:00:00Z"
night="2026-11-01T22:00:00Z"
lit="brightness: 100 (pwm 100, enable gpio43 high)"
dark="brightness: 0 (pwm 0, enable gpio43 low)"

wait_for_chips() {
  wait_for_output "power: charger: pre 01, charge 04, term 11, volt 02 (ESP_OK)"
  wait_for_output "boot rotation 0 from the sensor"
}

# The enable line sits low from power-on, held by the mod's pull-down, and
# backlight_hold_off() drives it low without touching the PWM. So the driver shows
# 100 with the line low until the firmware first sets a level, about 500 ms in.
start_run_at "$noon" --board "$mod_board"
send "@sim brightness"
wait_for_output "brightness: 0 (pwm 100, enable gpio43 low)"
wait_for_chips

# A daytime boot lights the screen by itself, with no fake clock.
wait_for_output "desert: "
send "phase"
wait_for_output "2026-11-01 12:00"
wait_for_output " local, day"
wait_for_shot picture
wait_for_brightness "$lit"

send "settings"
wait_for_output "wifi off"
send "menu 4"
wait_for_output "menu: open"
send "@sim touch 410 102"
send "settings"
for tenths in $(seq 1 50); do
  grep -qF "wifi on" "$app_log" && break
  sleep 0.1
  send "settings"
done
wait_for_output "wifi on"

send "wifi scan"
wait_for_output "Preflash Fake Home"

send "@sim audio"
wait_for_output "audio samples written: "
samples_before="$(last_number "audio samples written: ")"
send "chime 0"
wait_for_output "amp on while playing"
send "@sim audio"
wait_for_output "audio samples written: " 2
samples_after="$(last_number "audio samples written: ")"
[ "$samples_after" -gt "$samples_before" ] \
  || fail "the chime wrote no sound: $samples_before then $samples_after" "$(cat "$app_log")"
quit_app

# Saved state survives a run, and the display turns with the menu on it.
start_run_at "$noon" --board "$mod_board"
wait_for_chips
send "settings"
wait_for_output "wifi on"
send "orient 90"
wait_for_output "holding 90"
wait_for_output "orient: rotation 90"
send "menu 4"
wait_for_output "menu: open"
send "@sim touch 40 45"
wait_for_output "menu: closed"

# A lit screen goes dark when the clock jumps into the night.
send "fake $night"
wait_for_output "fake clock at $night"
wait_for_shot black
wait_for_brightness "$dark"
quit_app

# A night-time boot stays dark. A jump in the clock never lights a dark screen, however
# late it comes: that takes a tap.
start_run_at "$night" --board "$mod_board"
wait_for_chips
wait_for_shot black
wait_for_brightness "$dark"
sleep 3
send "fake $noon"
wait_for_output "fake clock at $noon"
sleep 1
wait_for_brightness "$dark"
tap_until_bright "$lit"
wait_for_shot picture
quit_app

# The Mac's own clock, at whatever hour this runs: the RTC reads it through the real
# driver, and a late fake clock and a tap reach the day from any phase. The same steps
# from a noon RTC cover the hours when this boot is already lit.
start_run --board "$mod_board"
wait_for_chips
send "rtc"
for tenths in $(seq 1 50); do
  reading="$(grep -E '^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:]{8}Z$' "$app_log" | tail -n 1 || true)"
  [ -n "$reading" ] && break
  sleep 0.1
done
[ -n "$reading" ] || fail "no time from rtc" "$(cat "$app_log")"
drift="$(($(date -u +%s) - $(date -j -u -f '%Y-%m-%dT%H:%M:%SZ' "$reading" +%s)))"
[ "${drift#-}" -le 2 ] || fail "rtc is $drift s from the Mac's clock: $reading"
sleep 3
send "fake $noon"
wait_for_output "fake clock at $noon"
tap_until_bright "$lit"
wait_for_shot picture
quit_app

start_run_at "$noon" --board "$mod_board"
wait_for_chips
sleep 3
send "fake $noon"
wait_for_output "fake clock at $noon"
tap_until_bright "$lit"
wait_for_shot picture
quit_app

# The stock board has no enable pin, so the same low line blanks nothing.
start_run_at "$noon"
send "@sim brightness"
wait_for_output "brightness: 100 (pwm 100)"
wait_for_chips
wait_for_brightness "brightness: 100 (pwm 100)"
if grep -qF "enable gpio" "$app_log"; then
  fail "the stock board reports an enable pin" "$(cat "$app_log")"
fi
quit_app

after="$(git -C "$copy" status --porcelain --ignored)"
[ -z "$(git -C "$copy" status --porcelain)" ] || fail "the project changed" "$after"
[ "$before" = "$after" ] || fail "the project gained files" "$after"

echo "PASS: snoozel"

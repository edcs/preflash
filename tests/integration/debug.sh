#!/usr/bin/env bash
# Boots an unmodified project headless, watches what it does to the hardware through
# preflash trace, and forces failures on it through preflash hardware. LVGL comes from the
# test suite's download, so build that first.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$root/tests/integration/app.sh"

lvgl="$root/build/tests/_deps/lvgl-src"
[ -f "$lvgl/lvgl.h" ] || { echo "FAIL: no LVGL in $lvgl: build tests/runtime first" >&2; exit 1; }

scratch="$(mktemp -d)"
app_pid=""
tracer=""
stop_all() {
  [ -z "$app_pid" ] || kill -9 "$app_pid" 2>/dev/null || true
  [ -z "$tracer" ] || kill -9 "$tracer" 2>/dev/null || true
  rm -rf "$scratch"
}
trap stop_all EXIT

fail() {
  echo "FAIL: $1" >&2
  [ -n "${2:-}" ] && printf '%s\n' "$2" >&2
  exit 1
}

# Kept between runs, so that only what changed gets rebuilt.
project="$root/build/integration/debug/debug_fixture"
mkdir -p "$project/managed_components"
rsync -a "$root/tests/fixtures/lvgl_project/" "$project/"
rsync -a --exclude /demos --exclude /docs --exclude /examples --exclude /tests \
  "$lvgl/" "$project/managed_components/lvgl__lvgl/"

go build -C "$root" -o "$scratch/preflash" ./cmd/preflash
pf() { "$scratch/preflash" "$@"; }

real_project="$(cd "$project" && pwd -P)"
state="$HOME/.preflash/state/debug_fixture-$(printf '%s' "$real_project" | shasum -a 256 | cut -c1-8)"
rm -rf "$state"

wait_in_file() {
  local text="$1" file="$2" tenths
  for tenths in $(seq 1 100); do
    grep -qF -- "$text" "$file" 2>/dev/null && return 0
    sleep 0.1
  done
  fail "timed out waiting for $text in $file" "$(tail -c 2000 "$file" 2>/dev/null)"
}

expect() {
  local text="$1" output="$2"
  case "$output" in
    *"$text"*) ;;
    *) fail "expected: $text" "$output" ;;
  esac
}

expect_failure() {
  local text="$1" output
  shift
  if output="$("$@" 2>&1)"; then fail "this should have failed: $*" "$output"; fi
  expect "$text" "$output"
}

# The first build can take longer than a test's wait for output.
pf run --project "$project" --headless <<< "@sim quit" || fail "the project didn't build and run"

expect_failure "no app is running" pf trace --project "$project"
expect_failure "no app is running" pf hardware --project "$project"
expect_failure "no stream" pf trace --stream tea --project "$project"

start_app "$scratch/preflash" run --project "$project" --headless
wait_for_output "ready, reset reason 1"

# Rows for every stream, as text and as JSON.
trace_log="$scratch/trace.log"
pf trace --project "$project" --json > "$trace_log" 2> "$scratch/trace.err" &
tracer=$!
wait_in_file_or_retry() {
  local text="$1" line="$2" tries
  for tries in $(seq 1 50); do
    send "$line"
    sleep 0.1
    grep -qF -- "$text" "$trace_log" 2>/dev/null && return 0
  done
  fail "no $text in the trace" "$(tail -c 2000 "$trace_log")"
}
wait_in_file_or_retry '"device":"PCF85063"' rtc
send save
wait_for_output "fixture: saved"
send pin
wait_for_output "pin set"
send spawn
wait_for_output "spawned"
send fetch
wait_for_output "fetched, err"
wait_in_file '"op":"commit"' "$trace_log"
wait_in_file '"subject":"fixture/greeting"' "$trace_log"
wait_in_file '"subject":"gpio7"' "$trace_log"
wait_in_file '"subject":"spawned"' "$trace_log"
wait_in_file '"subject":"http"' "$trace_log"
grep -qF hello "$trace_log" && fail "the trace shows an NVS value" "$(cat "$trace_log")"

gpio_only="$scratch/gpio.log"
pf trace --stream gpio --project "$project" > "$gpio_only" &
gpio_tracer=$!
send pin
wait_in_file "gpio7" "$gpio_only"
kill "$gpio_tracer"
wait "$gpio_tracer" 2>/dev/null || true
grep -q PCF85063 "$gpio_only" && fail "--stream let another stream through" "$(cat "$gpio_only")"

output="$(pf trace --save "$scratch/saved.jsonl" --project "$project")"
expect "records to $scratch/saved.jsonl" "$output"
python3 -I -c '
import json, sys
rows = [json.loads(line) for line in open(sys.argv[1])]
assert any(r["stream"] == "i2c" and r["device"] == "PCF85063" for r in rows), rows
' "$scratch/saved.jsonl" || fail "the saved trace is wrong" "$(cat "$scratch/saved.jsonl")"

# The other two reads.
output="$(python3 -I - "$(cd "$project" && pwd -P)" <<'PY'
import hashlib, json, os, socket, sys
digest = hashlib.sha256(sys.argv[1].encode()).hexdigest()[:12]
path = os.path.expanduser(f"~/Library/Caches/preflash/{digest}.sock")
s = socket.socket(socket.AF_UNIX)
s.connect(path)
f = s.makefile("rw")
for op in ("tasks", "nvs"):
    f.write(json.dumps({"id": 1, "op": op}) + "\n")
    f.flush()
    print(f.readline().strip())
PY
)"
expect '"name":"spawned"' "$output"
expect '"stack":2560' "$output"
expect '"key":"greeting"' "$output"

# What a boot would see, unchanged.
output="$(pf hardware --project "$project")"
expect "plugged in" "$output"
expect "not fitted" "$output"
expect "Mac's connection" "$output"
pf hardware --json --project "$project" | python3 -I -c '
import json, sys
items = {i["name"]: i for i in json.load(sys.stdin)}
assert items["rtc"]["applies"] == "boot", items
assert items["rotation"]["applies"] == "now", items
' || fail "hardware --json is wrong"
expect_failure "no hardware called toaster" pf hardware toaster on --project "$project"
expect_failure "rtc takes" pf hardware rtc soggy --project "$project"
expect_failure "no power" pf hardware usb unplugged --project "$project"

# Rotation and power reach the driver as it runs.
send accel
wait_for_output "accel err 0 x 0 y 16384"
output="$(pf hardware rotation 90 --project "$project")"
expect "rotation: 90°" "$output"
send accel
wait_for_output "accel err 0 x 16384 y 0"
pf hardware rotation 0 --project "$project" > /dev/null
pf hardware battery 50 --project "$project" > /dev/null
pf hardware usb unplugged --project "$project" > /dev/null
send power
wait_for_output "power err 0 status 08 gauge 50"

# A forced NACK arrives as the failure firmware would get, and the trace says who did it.
pf hardware i2c pcf85063 --project "$project" > /dev/null
send rtc
wait_for_output "rtc err 259 stopped 0"
wait_in_file '"detail":"NACK (injected)"' "$trace_log"
pf hardware i2c healthy --project "$project" > /dev/null

# Offline HTTP is a failure of the same kind.
pf hardware internet offline --project "$project" > /dev/null
send fetch
wait_in_file 'offline (injected)' "$trace_log"
pf hardware internet real --project "$project" > /dev/null

# A flat cell waits for the boot, which the restart here is, and the trace comes back with it.
output="$(pf hardware rtc flat --project "$project")"
expect "flat backup cell on the next boot" "$output"
output="$(pf hardware motion gone --project "$project")"
expect "gone on the next boot" "$output"
expect '"rtc":"flat"' "$(cat "$state/faults.json")"
expect "flat backup cell on the next boot" "$(pf hardware --project "$project")"
send rtc
wait_for_output "rtc err 0 stopped 0"
send restart
wait_for_output "ready, reset reason 3"
wait_for_output "forced on this boot"
send rtc
wait_for_output "rtc err 0 stopped 1"
send accel
wait_for_output "accel err 259"
wait_in_file '"subject":"0x6b"' "$trace_log"

kill "$tracer"
wait "$tracer" 2>/dev/null || true
tracer=""

# A new process honours the file too.
quit_app
start_app "$scratch/preflash" run --project "$project" --headless
wait_for_output "ready, reset reason 1"
expect "forced on this boot" "$(cat "$app_log")"
send rtc
wait_for_output "rtc err 0 stopped 1"
quit_app
rm -rf "$state"

echo "PASS: debug"

#!/usr/bin/env bash
# Boots an unmodified LVGL project headless with preflash run and drives it from outside,
# through the command line and the control socket. LVGL comes from the test suite's
# download, so build that first.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$root/tests/integration/app.sh"

lvgl="$root/build/tests/_deps/lvgl-src"
[ -f "$lvgl/lvgl.h" ] || { echo "FAIL: no LVGL in $lvgl: build tests/runtime first" >&2; exit 1; }

scratch="$(mktemp -d)"
app_pid=""
follower=""
stuck=""
stop_all() {
  [ -z "$app_pid" ] || kill -9 "$app_pid" 2>/dev/null || true
  [ -z "$follower" ] || kill -9 "$follower" 2>/dev/null || true
  [ -z "$stuck" ] || kill -9 "$stuck" 2>/dev/null || true
  rm -rf "$scratch"
}
trap stop_all EXIT

fail() {
  echo "FAIL: $1" >&2
  [ -n "${2:-}" ] && printf '%s\n' "$2" >&2
  exit 1
}

# Kept between runs, so that only what changed gets rebuilt.
project="$root/build/integration/control/control_fixture"
mkdir -p "$project/managed_components"
rsync -a "$root/tests/fixtures/lvgl_project/" "$project/"
rsync -a --exclude /demos --exclude /docs --exclude /examples --exclude /tests \
  "$lvgl/" "$project/managed_components/lvgl__lvgl/"

go build -C "$root" -o "$scratch/preflash" ./cmd/preflash
pf() { "$scratch/preflash" "$@"; }

real_project="$(cd "$project" && pwd -P)"
sum="$(printf '%s' "$real_project" | shasum -a 256 | cut -c1-12)"
socket="$HOME/Library/Caches/preflash/$sum.sock"

start_run() { start_app "$scratch/preflash" run --project "$project" --headless; }

# Waits for text in a file some background client is writing.
wait_in_file() {
  local text="$1" file="$2" tenths
  for tenths in $(seq 1 100); do
    grep -qF -- "$text" "$file" 2>/dev/null && return 0
    sleep 0.1
  done
  fail "timed out waiting for $text in $file" "$(tail -c 2000 "$file" 2>/dev/null)"
}

expect_failure() {
  local text="$1" output
  shift
  if output="$("$@" 2>&1)"; then fail "this should have failed: $*" "$output"; fi
  case "$output" in
    *"$text"*) ;;
    *) fail "the failure doesn't say: $text" "$output" ;;
  esac
}

# Follows the stream in the background, into $follow_log.
start_follower() {
  follow_log="$(mktemp "$scratch/follow.XXXXXX")"
  pf serial --project "$project" > "$follow_log" 2> "$follow_log.err" &
  follower=$!
}

# A follower ends by itself when the app stops, however it stopped.
follower_ended() {
  local tenths
  for tenths in $(seq 1 100); do
    kill -0 "$follower" 2>/dev/null || break
    sleep 0.1
  done
  kill -0 "$follower" 2>/dev/null && fail "the follower outlived the app"
  wait "$follower" || fail "the follower exited with an error" "$(cat "$follow_log.err")"
  follower=""
}

# The first build can take longer than a test's wait for output.
pf run --project "$project" --headless <<< "@sim quit" || fail "the project didn't build and run"
[ ! -e "$socket" ] || fail "quitting left the socket behind"

# No app is running.
for words in "status" "send ping" "serial" "touch 1 1" "quit" "shot $scratch/none.png"; do
  # shellcheck disable=SC2086
  set -- $words
  command="$1"
  shift
  expect_failure "no app is running" pf "$command" --project "$project" "$@"
done

start_run
wait_for_output "ready, reset reason 1"
[ "$(stat -f %Lp "$socket")" = 600 ] || fail "the socket's mode is $(stat -f %Lp "$socket")"

# Status, read by a person and by a script.
output="$(pf status --project "$project")"
case "$output" in
  *"control_fixture"*"ESP32-S3-Touch-LCD-4B (480x480)"*"$app_pid"*) ;;
  *) fail "status lacks something" "$output" ;;
esac
pf status --json --project "$project" | python3 -I -c '
import json, sys
s = json.load(sys.stdin)
assert s["project"] == "control_fixture", s
assert s["board"] == "ESP32-S3-Touch-LCD-4B", s
assert (s["width"], s["height"]) == (480, 480), s
assert s["pid"] == int(sys.argv[1]), s
assert s["uptime_ms"] > 0, s
assert s["backlight"] == 100, s
assert s["frames"] > 0, s
assert s["fps"] >= 0, s
assert s["wifi"] == "off", s
' "$app_pid" || fail "status --json is wrong"

# The socket and stdin feed one console, and the terminal still shows everything.
pf send --project "$project" ping
wait_for_output "pong"
send "ping"
wait_for_output "pong" 2

# A late client gets what it missed, then what comes next.
start_follower
wait_in_file "ready, reset reason 1" "$follow_log"
pf send --project "$project" ping
wait_in_file "pong" "$follow_log"

# A shot from the socket matches the runtime's own, and a PNG holds the same panel.
send "@sim shot $scratch/runtime.bmp"
wait_for_output "saved $scratch/runtime.bmp"
pf shot "$scratch/socket.bmp" --project "$project"
cmp "$scratch/runtime.bmp" "$scratch/socket.bmp" || fail "the two shots differ"
pf shot --project "$project" "$scratch/socket.png"
python3 -I - "$scratch/socket.png" <<'PY' || fail "bad PNG"
import struct, sys
data = open(sys.argv[1], "rb").read()
assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
assert struct.unpack(">II", data[16:24]) == (480, 480), "wrong size"
PY
expect_failure ".png or a .bmp" pf shot "$scratch/socket.gif" --project "$project"

pf touch 100 85 --project "$project"
wait_for_output "clicked"
pf touch 100 85 0 --project "$project"
wait_for_output "clicked" 2
expect_failure "on the panel" pf touch 9999 0 --project "$project"
expect_failure "numbers" pf touch one two --project "$project"

# A second app in the same project says who has the socket, and stops.
output="$(pf run --project "$project" --headless < /dev/null 2>&1)" \
  && fail "a second app ran" "$output"
case "$output" in
  *"control_fixture (pid $app_pid) already holds the socket"*) ;;
  *) fail "the second app didn't name the holder" "$output" ;;
esac
pf status --project "$project" > /dev/null || fail "a second app disturbed the first"

# A client that stops reading loses its oldest output and hears how much. The firmware
# doesn't wait for it, and neither does a client that keeps up.
python3 -I - "$socket" "$app_log" > "$scratch/stuck.out" 2>&1 <<'PY' &
import base64, json, socket, sys, time
client = socket.socket(socket.AF_UNIX)
client.connect(sys.argv[1])
stream = client.makefile("rb")
client.sendall(b'{"id":1,"op":"subscribe","streams":["serial"]}\n')
assert json.loads(stream.readline())["ok"]
print("subscribed", flush=True)
# Reads nothing until the firmware has finished, however long that takes it.
for _ in range(600):
    if b"flood done" in open(sys.argv[2], "rb").read():
        break
    time.sleep(0.1)
else:
    sys.exit("the firmware waited for a stuck client")
dropped = 0
tail = b""
client.settimeout(5)
while b"flood done" not in tail:
    event = json.loads(stream.readline())
    if event["event"] == "dropped":
        dropped += event["bytes"]
    elif event["event"] == "serial":
        tail = (tail + base64.b64decode(event["data"]))[-4096:]
assert dropped > 0, "nothing was dropped"
print("dropped", dropped, flush=True)
PY
stuck=$!
wait_in_file "subscribed" "$scratch/stuck.out"
pf send --project "$project" flood
wait_for_output "flood done"
wait_in_file "flood done" "$follow_log"
wait "$stuck" || fail "the stuck client" "$(cat "$scratch/stuck.out")"
stuck=""
grep -q "^dropped [0-9]" "$scratch/stuck.out" || fail "no drop reported" "$(cat "$scratch/stuck.out")"

# A restart keeps the process and its real stdout, and clients come back by themselves.
pid_before="$app_pid"
pf send --project "$project" restart
wait_for_output "ready, reset reason 3"
wait_in_file "ready, reset reason 3" "$follow_log"
pf status --json --project "$project" | python3 -I -c '
import json, sys
s = json.load(sys.stdin)
assert s["pid"] == int(sys.argv[1]), s
' "$pid_before" || fail "the restart changed the pid"
pongs="$(grep -cF pong "$app_log")"
pf send --project "$project" ping
wait_for_output "pong" $((pongs + 1))
send "ping"
wait_for_output "pong" $((pongs + 2))

# A new client gets this boot's output from its start.
"$scratch/preflash" serial --project "$project" > "$scratch/late.out" 2>&1 &
late=$!
wait_in_file "ready, reset reason 3" "$scratch/late.out"
kill "$late"
wait "$late" 2>/dev/null || true

# A command that never returns holds up the console queue and nothing else.
pf send --project "$project" hang
wait_for_output "hanging"
pf send --project "$project" ping
pf status --project "$project" > /dev/null || fail "status stuck behind a command"
pf touch 100 85 --project "$project" || fail "touch stuck behind a command"
pf shot "$scratch/stuck.png" --project "$project" || fail "shot stuck behind a command"
pf quit --project "$project" || fail "quit stuck behind a command"
[ ! -e "$socket" ] || fail "quit left the socket behind"
follower_ended
finish_app
[ "$app_status" -eq 0 ] || fail "quit exited with status $app_status"
expect_failure "no app is running" pf status --project "$project"

# The same hang, with quit coming down stdin.
start_run
wait_for_output "ready, reset reason"
send "hang"
wait_for_output "hanging"
quit_app

# @sim quit sent over the socket gets its reply before the process goes.
start_run
wait_for_output "ready, reset reason"
pf send --project "$project" "@sim" quit || fail "send of @sim quit failed"
finish_app
[ "$app_status" -eq 0 ] || fail "@sim quit exited with status $app_status"

# The message that says why the process stopped reaches a client.
start_run
wait_for_output "ready, reset reason"
start_follower
wait_in_file "ready, reset reason" "$follow_log"
pf send --project "$project" unsupported
wait_in_file "esp_console_repl_del is not supported in the simulator" "$follow_log"
wait_for_output "esp_console_repl_del is not supported in the simulator"
follower_ended
finish_app
[ "$app_status" -ne 0 ] || fail "an unsupported call exited cleanly"

# That left the socket behind. It answers no one, and the next app takes it.
[ -S "$socket" ] || fail "expected the dead app's socket to remain"
expect_failure "no app is running" pf status --project "$project"
start_run
wait_for_output "ready, reset reason"
kill -INT "$app_pid"
finish_app
[ "$app_status" -eq 0 ] || fail "SIGINT exited with status $app_status" "$(cat "$app_log")"
[ ! -e "$socket" ] || fail "SIGINT left the socket behind"

echo "PASS: control"

#!/usr/bin/env bash
# Runs preflash web on the demo and drives it as a browser would: the page, the terminal app's
# start, the device's frames, a tap, a button, the beep's sound, the host check, and a clean ^C.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$root/tests/integration/app.sh"

scratch="$(mktemp -d)"
app_pid=""
real_project=""
# Stops preflash web with ^C, as a person would, so that it stops its terminal app and the
# firmware too. Only an app that ignores it is killed.
stop_all() {
  if [ -n "$app_pid" ]; then
    kill -INT "$app_pid" 2>/dev/null || true
    wait_for_exit 150 || kill -9 "$app_pid" 2>/dev/null || true
  fi
  [ -z "$real_project" ] || pkill -f "$real_project" 2>/dev/null || true
  rm -rf "$scratch"
}
trap stop_all EXIT

fail() {
  echo "FAIL: $1" >&2
  [ -n "${2:-}" ] && printf '%s\n' "$2" >&2
  exit 1
}

go build -C "$root" -o "$scratch/preflash" ./cmd/preflash
go build -C "$root" -o "$scratch/webprobe" ./tests/integration/webprobe
pf() { "$scratch/preflash" "$@"; }

# Kept between runs, so that only what changed gets rebuilt.
project="$root/build/integration/web/demo"
mkdir -p "$project"
rsync -a --delete "$root/examples/demo/" "$project/"
real_project="$(cd "$project" && pwd -P)"
sum="$(printf '%s' "$real_project" | shasum -a 256 | cut -c1-8)"
rm -rf "$HOME/.preflash/state/demo-$sum"
rm -f "$HOME/.preflash/build/demo-$sum/crash.json"
socket="$HOME/Library/Caches/preflash/$(printf '%s' "$real_project" | shasum -a 256 | cut -c1-12).sock"
[ ! -e "$socket" ] || fail "an app is already running on the demo copy: $socket"

# Port 0, so that a port someone else holds doesn't matter.
start_app "$scratch/preflash" web --project "$project" --port 0
wait_for_output "Running on: http://localhost:"
[ "$(head -n 1 "$app_log")" = "preflash dev" ] || fail "the first line isn't preflash dev" "$(cat "$app_log")"
[ "$(sed -n 2p "$app_log")" = "" ] || fail "the second line isn't blank" "$(cat "$app_log")"
addr="localhost:$(sed -n 's|^Running on: http://localhost:\([0-9]*\)$|\1|p' "$app_log")"
[ "$addr" != "localhost:" ] || fail "no port in the URL" "$(cat "$app_log")"

code="$(curl -s -o "$scratch/page" -w '%{http_code}' "http://$addr/")"
[ "$code" = 200 ] || fail "the page answered $code"
grep -q '<html' "$scratch/page" || fail "the page isn't HTML" "$(head -c 300 "$scratch/page")"

# The server starts the terminal app itself, and a page's start message changes nothing while
# it runs. This checks that /terminal relays the app's output. The demo's .preflashrc names a
# board, so the launch screen is skipped and no key is needed.
"$scratch/webprobe" start "$addr" || fail "the terminal app didn't start" "$(cat "$app_log")"

# The first wait covers the build.
for tenths in $(seq 1 $(( ${PREFLASH_TEST_WAIT:-10} * 10 ))); do
  pf status --project "$project" > /dev/null 2>&1 && break
  kill -0 "$app_pid" 2>/dev/null || fail "preflash web stopped before the app answered" "$(cat "$app_log")"
  sleep 0.1
done
pf status --project "$project" > /dev/null 2>&1 || fail "the app never answered status" "$(cat "$app_log")"

"$scratch/webprobe" device "$addr" "$scratch/preflash" "$project" || fail "the device stream check failed" "$(cat "$app_log")"

# The demo's beep reaches /audio as a format message and Opus packets, the first of them a burst
# start. The probe hears the format, which means the relay is subscribed, before the beep goes out.
"$scratch/webprobe" audio "$addr" "$scratch/preflash" "$project" || fail "the sound check failed" "$(cat "$app_log")"

# A page from another origin is refused.
code="$(curl -s -o /dev/null -w '%{http_code}' \
  -H 'Origin: https://evil.example' -H 'Connection: Upgrade' -H 'Upgrade: websocket' \
  -H 'Sec-WebSocket-Version: 13' -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' \
  "http://$addr/device")"
[ "$code" = 403 ] || fail "a foreign origin got $code, not 403"

# ^C stops the terminal app and the firmware with it.
kill -INT "$app_pid"
wait_for_exit 150 || fail "preflash web was still running 15 s after ^C" "$(cat "$app_log")"
wait "$app_pid" 2>/dev/null || true
app_pid=""
exec 3>&-
for tenths in $(seq 1 150); do
  pgrep -f "$real_project" > /dev/null || break
  sleep 0.1
done
left="$(pgrep -fl "$real_project" || true)"
[ -z "$left" ] || fail "a process was left behind" "$left"
[ ! -e "$socket" ] || fail "the socket file was left behind: $socket"

echo PASS

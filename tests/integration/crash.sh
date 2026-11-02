#!/usr/bin/env bash
# Crashes an unmodified LVGL project each way it can go, reads the reports back with preflash
# crash, and does the same under each sanitizer. LVGL comes from the test suite's download.
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

# Kept between runs, so that only what changed gets rebuilt.
project="$root/build/integration/crash/crash_fixture"
mkdir -p "$project/managed_components"
rsync -a "$root/tests/fixtures/lvgl_project/" "$project/"
rsync -a --exclude /demos --exclude /docs --exclude /examples --exclude /tests \
  "$lvgl/" "$project/managed_components/lvgl__lvgl/"
# As a project on LVGL's own allocator has it, which a sanitizer can't see into.
sed -i '' -e 's/^CONFIG_LV_USE_CLIB_MALLOC=y/CONFIG_LV_USE_BUILTIN_MALLOC=y\nCONFIG_LV_MEM_SIZE_KILOBYTES=256/' \
  "$project/sdkconfig"

go build -C "$root" -o "$scratch/preflash" ./cmd/preflash
pf() { "$scratch/preflash" "$@"; }

real_project="$(cd "$project" && pwd -P)"
sum="$(printf '%s' "$real_project" | shasum -a 256 | cut -c1-12)"
build_dir="$HOME/.preflash/build/$(basename "$real_project")-$(printf '%s' "$real_project" | shasum -a 256 | cut -c1-8)"
rm -f "$build_dir"/crash.json "$build_dir"-address/crash.json "$build_dir"-thread/crash.json

expect_in() {
  case "$2" in
    *"$1"*) ;;
    *) fail "lacks: $1" "$2" ;;
  esac
}

# Runs headless until the console's command crashes it, which ends the process.
crash_with() {
  local line="$1"
  shift
  start_app "$scratch/preflash" run --project "$project" --headless "$@"
  wait_for_output "ready, reset reason"
  pf send --project "$project" "$line"
  wait_for_exit 300 || fail "$line left the app running" "$(cat "$app_log")"
  finish_app
  [ "$app_status" -ne 0 ] || fail "$line exited cleanly" "$(cat "$app_log")"
  report="$(pf crash --project "$project")"
}

expect_failure() {
  local text="$1" output
  shift
  if output="$("$@" 2>&1)"; then fail "this should have failed: $*" "$output"; fi
  expect_in "$text" "$output"
}

expect_failure "no crash report" pf crash --project "$project"

crash_with null
expect_in "Null pointer read in" "$report"
expect_in "null_command" "$report"
expect_in "main/main.cpp:" "$report"
expect_in "memory error on the console task" "$report"
expect_in "crashed on console: SIGSEGV" "$(cat "$app_log")"

crash_with abort
expect_in "abort() called in" "$report"
expect_in "abort_command" "$report"

crash_with check
expect_in "ESP_ERROR_CHECK failed: ESP_ERR_NO_MEM" "$report"
expect_in "check_command" "$report"
expect_in "expression: ESP_ERR_NO_MEM" "$(cat "$app_log")"

crash_with panic
expect_in "esp_system_abort: heap corrupt" "$report"

crash_with unsupported
expect_in "esp_console_repl_del is not supported in the simulator" "$report"
expect_in "unsupported_command" "$report"

# JSON for a script, with what the app wrote and what Go made of it.
pf crash --project "$project" --json | grep -q '"signal":"SIGABRT"' || fail "no JSON report"

# With a window, a crash holds: the socket and the last frame stay, and a quit ends it. The
# dummy driver runs SDL's window code with nothing on the screen.
SDL_VIDEODRIVER=dummy start_app "$scratch/preflash" run --project "$project"
wait_for_output "ready, reset reason"
sleep 1
pf shot --project "$project" "$scratch/before.png"
pf send --project "$project" null
wait_for_output "crashed on console"
sleep 0.5
kill -0 "$app_pid" 2>/dev/null || fail "a windowed crash ended the process" "$(cat "$app_log")"
expect_in '"crashed":true' "$(pf status --project "$project" --json)"
expect_failure "the app has crashed" pf send --project "$project" ping
pf shot --project "$project" "$scratch/after.png"
cmp -s "$scratch/before.png" "$scratch/after.png" || fail "the last frame changed after the crash"
pf quit --project "$project"
finish_app

# A sanitizer's report joins the crash report, with its own stack, and the plain build stays.
plain_app="$build_dir/app"
plain_sum="$(shasum "$plain_app")"
crash_with overflow --sanitize address
expect_in "heap-buffer-overflow in" "$report"
expect_in "overflow_command" "$report"
expect_in "main/main.cpp:" "$report"
expect_in "AddressSanitizer: heap-buffer-overflow" "$(cat "$app_log")"
[ -x "$build_dir-address/app" ] || fail "no build of its own for address"
[ "$(shasum "$plain_app")" = "$plain_sum" ] || fail "a sanitized build changed the plain one"

crash_with race --sanitize thread
expect_in "data race in" "$report"
expect_in "race_task" "$report"
expect_in "ThreadSanitizer: data race" "$(cat "$app_log")"

echo "PASS: crash"

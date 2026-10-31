# Runs a built app in the background and talks to it over stdin, as a person
# would, because it never exits by itself. Sourced by the integration scripts,
# which define fail().

# Starts an app. Its output collects in $app_log and fd 3 writes to its stdin.
start_app() {
  app_log="$(mktemp "$scratch/log.XXXXXX")"
  app_in="$(mktemp -u "$scratch/in.XXXXXX")"
  mkfifo "$app_in"
  "$@" < "$app_in" > "$app_log" 2>&1 &
  app_pid=$!
  exec 3> "$app_in"
}

send() {
  printf '%s\n' "$1" >&3
}

# Waits for the app to print something, as many times as asked.
wait_for_output() {
  local text="$1" times="${2:-1}" tenths
  for tenths in $(seq 1 100); do
    [ "$(grep -cF -- "$text" "$app_log" || true)" -ge "$times" ] && return 0
    kill -0 "$app_pid" 2>/dev/null || fail "the app stopped before printing: $text" "$(cat "$app_log")"
    sleep 0.1
  done
  fail "timed out waiting for: $text" "$(cat "$app_log")"
}

# Closes stdin and collects the exit status in $app_status.
finish_app() {
  exec 3>&-
  app_status=0
  wait "$app_pid" || app_status=$?
  app_pid=""
}

quit_app() {
  send "@sim quit"
  finish_app
  [ "$app_status" -eq 0 ] || fail "the app quit with status $app_status" "$(cat "$app_log")"
}

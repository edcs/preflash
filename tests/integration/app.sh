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

# Waits for the app to print something, as many times as asked. The first wait covers the
# build too, so a slow machine can allow more than 10 s with PREFLASH_TEST_WAIT, in seconds.
wait_for_output() {
  local text="$1" times="${2:-1}" tenths
  for tenths in $(seq 1 $(( ${PREFLASH_TEST_WAIT:-10} * 10 ))); do
    [ "$(grep -cF -- "$text" "$app_log" || true)" -ge "$times" ] && return 0
    kill -0 "$app_pid" 2>/dev/null || fail "the app stopped before printing: $text" "$(cat "$app_log")"
    sleep 0.1
  done
  fail "timed out waiting for: $text" "$(cat "$app_log")"
}

# Gives the app some tenths of a second to end by itself. False if it's still up.
wait_for_exit() {
  local tenths
  for tenths in $(seq 1 "$1"); do
    kill -0 "$app_pid" 2>/dev/null || return 0
    sleep 0.1
  done
  ! kill -0 "$app_pid" 2>/dev/null
}

# Closes stdin and collects the exit status in $app_status. An app that is still up
# after ten seconds fails the script instead of hanging it.
finish_app() {
  exec 3>&-
  if ! wait_for_exit 100; then
    kill -9 "$app_pid" 2>/dev/null || true
    wait "$app_pid" 2>/dev/null || true
    app_pid=""
    fail "the app was still running 10 s after it was told to stop" "$(cat "$app_log")"
  fi
  app_status=0
  wait "$app_pid" || app_status=$?
  app_pid=""
}

quit_app() {
  send "@sim quit"
  finish_app
  [ "$app_status" -eq 0 ] || fail "the app quit with status $app_status" "$(cat "$app_log")"
}

# What differs between macOS and Linux, in one place, so the scripts read the same on both.

# Edits a file where it is: BSD sed wants a backup suffix, even an empty one, and GNU sed
# takes the empty string for the script.
sed_in_place() {
  if [ "$(uname)" = Darwin ]; then sed -i '' "$@"; else sed -i "$@"; fi
}

# The permission bits of what a path leads to, in octal, as 600.
file_mode() {
  if [ "$(uname)" = Darwin ]; then stat -L -f %Lp "$1"; else stat -L -c %a "$1"; fi
}

# Where Preflash keeps its sockets and port links, worked out as preflash does.
cache_dir() {
  if [ "$(uname)" = Darwin ]; then
    echo "$HOME/Library/Caches/preflash"
  else
    case "${XDG_CACHE_HOME:-}" in
      /*) echo "${XDG_CACHE_HOME%/}/preflash" ;;
      *) echo "$HOME/.cache/preflash" ;;
    esac
  fi
}

# The SHA-256 of stdin, or of the files named, as shasum prints it. Linux may have only
# sha256sum, which prints the same. find -exec can't run a function, so it takes the array.
if command -v shasum > /dev/null; then sha256_command=(shasum -a 256); else sha256_command=(sha256sum); fi
sha256() {
  "${sha256_command[@]}" "$@"
}

# What a pseudo-terminal's device path starts with, as a grep pattern.
pty_pattern() {
  if [ "$(uname)" = Darwin ]; then echo '^/dev/ttys'; else echo '^/dev/pts/'; fi
}

# The program a process is running, as pgrep -f can find it.
exe_of() {
  if [ "$(uname)" = Darwin ]; then ps -o comm= -p "$1"; else readlink "/proc/$1/exe"; fi
}

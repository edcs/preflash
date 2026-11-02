#!/usr/bin/env bash
# Boots the LVGL fixture headless and talks to it the way a companion app would: through the
# virtual serial port, opened at 115200, alongside the socket's clients. Only the app's own
# pseudo-terminal is opened here. Run control.sh first, which builds the fixture.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$root/tests/integration/app.sh"

scratch="$(mktemp -d)"
app_pid=""
follower=""
companion=""
project="$root/build/integration/control/control_fixture"
stop_all() {
  [ -z "$app_pid" ] || kill -9 "$app_pid" 2>/dev/null || true
  [ -z "$follower" ] || kill -9 "$follower" 2>/dev/null || true
  [ -z "$companion" ] || kill -9 "$companion" 2>/dev/null || true
  rm -f "$project/.preflashrc"
  rm -rf "$project/links" "$scratch"
}
trap stop_all EXIT

fail() {
  echo "FAIL: $1" >&2
  [ -n "${2:-}" ] && printf '%s\n' "$2" >&2
  exit 1
}

[ -d "$project/main" ] || fail "no fixture in $project: run control.sh first"

go build -C "$root" -o "$scratch/preflash" ./cmd/preflash
pf() { "$scratch/preflash" "$@"; }

real_project="$(cd "$project" && pwd -P)"
sum="$(printf '%s' "$real_project" | shasum -a 256 | cut -c1-12)"
port="$HOME/Library/Caches/preflash/$sum.tty"

start_run() { start_app "$scratch/preflash" run --project "$project" --headless; }

wait_in_file() {
  local text="$1" file="$2" tenths
  for tenths in $(seq 1 100); do
    grep -qF -- "$text" "$file" 2>/dev/null && return 0
    sleep 0.1
  done
  fail "timed out waiting for $text in $file" "$(tail -c 2000 "$file" 2>/dev/null)"
}

# The companion app: stdin holds steps, one per line, and each step's outcome goes to stdout.
#   open         opens the port at 115200 in raw mode, as a serial library would
#   plain        opens it and leaves its settings alone
#   send <text>  writes text and a newline, or \r\n after "crlf"
#   crlf         ends the next send with a carriage return too
#   wait <text>  reads until text arrives, then forgets what it read
#   quiet <text> fails if text is what has arrived so far, then forgets it
#   eof          reads until the app's end of the port goes away
#   close        closes the port, as an app that quits would
cat > "$scratch/companion.py" <<'PY'
import errno, os, select, sys, termios, time

fd = -1
heard = b""
crlf = False

def read_some(wait):
    global heard
    ready, _, _ = select.select([fd], [], [], wait)
    if not ready:
        return True
    try:
        data = os.read(fd, 65536)
    except OSError as e:
        if e.errno == errno.EAGAIN:
            return True
        if e.errno == errno.EIO:
            return False
        raise
    heard += data
    return len(data) > 0

for step in sys.stdin:
    word, _, text = step.rstrip("\n").partition(" ")
    if word in ("open", "plain"):
        fd = os.open(sys.argv[1], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        if word == "open":
            attrs = termios.tcgetattr(fd)
            attrs[0] = attrs[1] = attrs[3] = 0
            attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
            attrs[4] = attrs[5] = termios.B115200
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
    elif word == "crlf":
        crlf = True
    elif word == "send":
        os.write(fd, text.encode() + (b"\r\n" if crlf else b"\n"))
        crlf = False
    elif word == "wait":
        deadline = time.time() + 15
        while text.encode() not in heard:
            if time.time() > deadline or not read_some(0.1):
                sys.exit(f"never heard {text!r}: {heard[-500:]!r}")
        heard = b""
    elif word == "quiet":
        read_some(0.3)
        if text.encode() in heard:
            sys.exit(f"heard {text!r}: {heard[-500:]!r}")
        heard = b""
    elif word == "eof":
        deadline = time.time() + 15
        while read_some(0.1):
            if time.time() > deadline:
                sys.exit("the port never hung up")
    elif word == "close":
        os.close(fd)
        fd = -1
    print("done", step.strip(), flush=True)
PY

# Starts the companion on a port, with fd 4 writing its steps.
start_companion() {
  companion_in="$(mktemp -u "$scratch/steps.XXXXXX")"
  companion_log="$(mktemp "$scratch/companion.XXXXXX")"
  mkfifo "$companion_in"
  steps=0
  python3 -I "$scratch/companion.py" "$1" < "$companion_in" > "$companion_log" 2>&1 &
  companion=$!
  exec 4> "$companion_in"
}

# Gives the companion a step and waits for it to finish. Steps repeat, so this counts
# them, and an earlier step's line in the log can't pass for this one's.
step() {
  local tenths
  steps=$((steps + 1))
  printf '%s\n' "$1" >&4
  for tenths in $(seq 1 200); do
    [ "$(grep -c '^done ' "$companion_log" || true)" -ge "$steps" ] && return 0
    kill -0 "$companion" 2>/dev/null || break
    sleep 0.1
  done
  fail "the companion didn't finish: $1" "$(cat "$companion_log")"
}

stop_companion() {
  exec 4>&-
  wait "$companion" || fail "the companion" "$(cat "$companion_log")"
  companion=""
}

# Makes sure the build is current, since this script changes nothing else about it.
pf run --project "$project" --headless <<< "@sim quit" || fail "the project didn't build and run"
[ ! -L "$port" ] || fail "quitting left the port's link behind"
expect_no_app="$(pf port --project "$project" 2>&1)" && fail "port with no app worked" "$expect_no_app"
case "$expect_no_app" in
  *"no app is running"*) ;;
  *) fail "port with no app doesn't say so" "$expect_no_app" ;;
esac

start_run
wait_for_output "ready, reset reason 1"

# The port has a name that stays the same from one run to the next.
[ "$(pf port --project "$project")" = "$port" ] || fail "preflash port printed something else" "$(pf port --project "$project")"
[ -L "$port" ] || fail "no link at $port"
device="$(readlink "$port")"
case "$device" in
  /dev/ttys*) ;;
  *) fail "the link points at $device, which isn't a pseudo-terminal" ;;
esac
[ "$(stat -L -f %Lp "$port")" = 600 ] || fail "the port's mode is $(stat -L -f %Lp "$port")"
pf status --json --project "$project" | python3 -I -c '
import json, sys
s = json.load(sys.stdin)
assert s["serial_port"] == sys.argv[1], s
' "$port" || fail "status has no serial_port"
pf status --project "$project" | grep -qF "$port" || fail "status doesn't show the port"

# A companion and a socket client share one stream, and either can drive it.
pf serial --project "$project" > "$scratch/follow" 2>&1 &
follower=$!
wait_in_file "ready, reset reason 1" "$scratch/follow"
start_companion "$port"
step "open"
step "send ping"
step "wait pong"
step "quiet ping"
wait_in_file "pong" "$scratch/follow"
wait_for_output "pong"
pongs="$(grep -cF pong "$app_log")"
pf send --project "$project" ping
step "wait pong"
send "ping"
step "wait pong"
wait_for_output "pong" $((pongs + 2))

# A line can end the way a terminal's or a Windows tool's does.
step "crlf"
step "send ping"
step "wait pong"
step "quiet Unrecognized"

# A restart keeps the port, so the same open descriptor carries on.
pid_before="$app_pid"
step "send restart"
step "wait ready, reset reason 3"
[ "$(pf status --json --project "$project" | python3 -I -c 'import json, sys; print(json.load(sys.stdin)["pid"])')" = "$pid_before" ] \
  || fail "the restart changed the pid"
[ "$(readlink "$port")" = "$device" ] || fail "the restart moved the port"
step "send ping"
step "wait pong"

# A quit left in the terminal during a restart works on the next boot. A client that has
# stopped reading stretches the restart, so that the line lands while nothing reads it.
python3 -I - "${port%.tty}.sock" <<'PY' &
import socket, sys, time
client = socket.socket(socket.AF_UNIX)
client.connect(sys.argv[1])
client.sendall(b'{"id":1,"op":"subscribe","streams":["serial"]}\n')
time.sleep(5)
PY
stuck=$!
step "send flood"
step "wait flood done"
step "send restart"
sleep 0.2
step "send @sim quit"
step "eof"
stop_companion
kill "$stuck" 2>/dev/null || true
wait "$stuck" 2>/dev/null || true
finish_app
[ "$app_status" -eq 0 ] || fail "that quit exited with status $app_status" "$(cat "$app_log")"
grep -qF "Unknown control" "$app_log" && fail "the quit reached a boot that couldn't run it" "$(cat "$app_log")"
kill "$follower" 2>/dev/null || true
wait "$follower" 2>/dev/null || true
follower=""
start_run
wait_for_output "ready, reset reason 1"
start_companion "$port"
step "open"

# Quitting hangs up, and takes the link with it.
pf quit --project "$project"
step "eof"
stop_companion
[ ! -L "$port" ] || fail "quit left the port's link behind"
finish_app

# With nobody attached, the firmware runs on, and a late companion finds a working port.
start_run
wait_for_output "ready, reset reason 1"
send "flood"
wait_for_output "flood done"
send "ping"
wait_for_output "pong"
start_companion "$port"
step "plain"
step "send ping"
step "wait pong"
step "close"
# A companion that comes back finds the settings as it left them: no echo, and raw.
step "plain"
step "send ping"
step "wait pong"
step "quiet ping"
step "close"
stop_companion

# A restart with nobody attached keeps the port too.
device="$(readlink "$port")"
send "restart"
wait_for_output "ready, reset reason 3"
[ "$(readlink "$port")" = "$device" ] || fail "the restart moved a port nobody had open"
start_companion "$port"
step "open"
step "send ping"
step "wait pong"
step "close"
stop_companion
quit_app
[ ! -L "$port" ] || fail "@sim quit left the port's link behind"

# A project can ask for a link of its own, relative to the project, for a companion app's
# developer setting.
mkdir -p "$project/links"
printf 'serial_link: links/fixture.tty\n' > "$project/.preflashrc"
start_run
wait_for_output "ready, reset reason 1"
[ -L "$project/links/fixture.tty" ] || fail "no link of the project's choosing"
[ "$(readlink "$project/links/fixture.tty")" = "$(readlink "$port")" ] || fail "the two links differ"
[ "$(pf port --project "$project")" = "$port" ] || fail "port moved to the project's link"
start_companion "$project/links/fixture.tty"
step "open"
step "send ping"
step "wait pong"
step "close"
stop_companion

# A crash takes the links too: the device's name goes back in the pool, and a link left
# behind could end up on someone's terminal.
pf send --project "$project" unsupported
wait_for_output "esp_console_repl_del is not supported in the simulator"
finish_app
[ ! -L "$port" ] || fail "a crash left the port's link behind"
[ ! -L "$project/links/fixture.tty" ] || fail "a crash left the project's link behind"

# Something else at the link's path is left alone, and the port still works.
echo keep > "$project/links/fixture.tty"
start_run
wait_for_output "ready, reset reason 1"
wait_for_output "isn't a link"
[ "$(cat "$project/links/fixture.tty")" = keep ] || fail "the project's file was replaced"
start_companion "$port"
step "open"
step "send ping"
step "wait pong"
step "close"
stop_companion
quit_app
[ "$(cat "$project/links/fixture.tty")" = keep ] || fail "quitting removed the project's file"
rm -f "$project/.preflashrc" "$project/links/fixture.tty"

# A process killed outright leaves links, and the next app takes them over.
start_run
wait_for_output "ready, reset reason 1"
kill -9 "$app_pid"
wait "$app_pid" 2>/dev/null || true
app_pid=""
exec 3>&-
[ -L "$port" ] || fail "expected the killed app's link to remain"
start_run
wait_for_output "ready, reset reason 1"
start_companion "$port"
step "open"
step "send ping"
step "wait pong"
step "close"
stop_companion
quit_app

echo "PASS: port"

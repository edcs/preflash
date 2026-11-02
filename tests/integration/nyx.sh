#!/usr/bin/env bash
# Runs a real project with no board package, Nyx, from a copy of its source: nothing here may
# change the firmware or write inside the copy. It's a private project, so this only runs with
# NYX pointing at a checkout or a copy that has run idf.py once, for sdkconfig and
# managed_components. tests/integration/lcd.sh covers the same ground from a fixture.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if [ -z "${NYX:-}" ]; then
  echo "SKIP: nyx: set NYX to a copy of it"
  exit 0
fi
source "$root/tests/integration/app.sh"

scratch="$(mktemp -d)"
app_pid=""
companion=""
trap '[ -z "$app_pid" ] || kill -9 "$app_pid" 2>/dev/null; [ -z "$companion" ] || kill -9 "$companion" 2>/dev/null; rm -rf "$scratch"' EXIT

fail() {
  echo "FAIL: $1" >&2
  [ -n "${2:-}" ] && printf '%s\n' "$2" >&2
  exit 1
}

# Kept between runs, so that only what changed gets rebuilt.
copy="$root/build/integration/nyx"
mkdir -p "$copy"
rsync -a --delete --exclude /build --exclude /build-host --exclude /mac "$NYX/" "$copy/"

matches="$(grep -ril nyx "$root/runtime" "$root/boards" || true)"
[ -z "$matches" ] || fail "the tool mentions the project" "$matches"

go build -C "$root" -o "$scratch/preflash" ./cmd/preflash
pf() { "$scratch/preflash" "$@"; }
board="ESP32-S3-Touch-LCD-3.49-V2"

snapshot() { (cd "$copy" && find . -path ./.git -prune -o -type f -exec shasum -a 256 {} + | sort -k2); }
before="$(snapshot)"

# The first build can take longer than a test's wait for output.
pf run --project "$copy" --board "$board" --headless <<< "@sim quit" \
  || fail "the project didn't build and run"

real_copy="$(cd "$copy" && pwd -P)"
sum="$(printf '%s' "$real_copy" | shasum -a 256 | cut -c1-12)"
port="$HOME/Library/Caches/preflash/$sum.tty"

# Prints how many colours a shot of the 172 by 640 panel has, and its hash.
shot_summary() {
  local path="$scratch/$1.bmp"
  pf shot --project "$copy" "$path"
  python3 -I - "$path" <<'PY'
import hashlib, struct, sys
data = open(sys.argv[1], "rb").read()
offset = struct.unpack_from("<I", data, 10)[0]
width, height = struct.unpack_from("<ii", data, 18)
assert (width, height) == (172, 640), f"size is {width}x{height}"
pixels = data[offset:]
colours = {pixels[i:i + 3] for i in range(0, len(pixels) - 2, 3)}
print(len(colours), hashlib.sha256(pixels).hexdigest())
PY
}

# The Mac app's part, over the virtual serial port: says hello, sends a meeting already under
# way, and waits for the bar to say the alarm was tapped.
cat > "$scratch/mac.py" <<'PY'
import os, select, sys, termios, time

fd = os.open(sys.argv[1], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
attrs = termios.tcgetattr(fd)
attrs[0] = attrs[1] = attrs[3] = 0
attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
attrs[4] = attrs[5] = termios.B115200
termios.tcsetattr(fd, termios.TCSANOW, attrs)

heard = b""

def send(line):
    os.write(fd, (line + "\n").encode())

def wait_for(text, timeout=20):
    global heard
    deadline = time.time() + timeout
    while text not in heard:
        if time.time() > deadline:
            sys.exit(f"never heard {text!r}: {heard[-300:]!r}")
        now = int(time.time())
        send(f"@T\t{now}\t0")  # the time keeps the link up, as the app's heartbeat does
        if select.select([fd], [], [], 0.2)[0]:
            heard += os.read(fd, 65536)
    heard = b""

now = int(time.time())
send("@?")
wait_for(b"@H\tnyx\t")
send(f"@T\t{now}\t0")
send("@S\t1")
send(f"@M\t1\tfake-1\t{now - 30}\t{now + 1800}\t0\tTeams\tSprint plan")
send("@E\t1\t1")
print("alarm sent", flush=True)
wait_for(b"@A\tfake-1\n", 60)
print("acknowledged", flush=True)
PY

start_app "$scratch/preflash" run --project "$copy" --board "$board" --headless
sleep 3  # past the splash, which ignores a tap

send "uptime"
wait_for_output ", reset reason 1"
send "power"
wait_for_output "PWR up"
send "imu"
wait_for_output "accel 0.000 1.000 0.000 g"
send "backlight"
wait_for_output "backlight 100%"
send "@sim brightness"
wait_for_output "brightness: 100 (pwm 100, enable exio1 high)"

# Rotation follows the console, and each way round draws a real picture.
read -r colours landscape <<< "$(shot_summary landscape)"
[ "$colours" -gt 4 ] || fail "the screen has only $colours colours" "$(cat "$app_log")"
send "rotate 1"
turned=""
for tenths in $(seq 1 30); do
  read -r colours turned <<< "$(shot_summary turned)"
  [ "$turned" != "$landscape" ] && break
  sleep 0.1
done
[ "$turned" != "$landscape" ] || fail "rotating changed nothing" "$(cat "$app_log")"
[ "$colours" -gt 4 ] || fail "the turned screen has only $colours colours" "$(cat "$app_log")"
send "rotate 0"

# The Mac's lines reach the parser, and a tap on the alarm goes back to the Mac.
python3 -I "$scratch/mac.py" "$port" > "$scratch/mac.log" 2>&1 &
companion=$!
for tenths in $(seq 1 100); do
  grep -qF "alarm sent" "$scratch/mac.log" && break
  sleep 0.1
done
grep -qF "alarm sent" "$scratch/mac.log" || fail "the bar never said hello" "$(cat "$scratch/mac.log")"
sleep 1
acknowledged=""
for tries in $(seq 1 20); do
  pf touch --project "$copy" 86 320
  sleep 0.5
  grep -qF "acknowledged" "$scratch/mac.log" && acknowledged=yes && break
done
[ -n "$acknowledged" ] || fail "a touch never acknowledged the alarm" "$(cat "$scratch/mac.log" "$app_log")"
wait "$companion" || fail "the Mac's part" "$(cat "$scratch/mac.log")"
companion=""

# On USB power, switching off finds the board still running, and carries on.
send "poweroff"
wait_for_output "still powered, so carrying on"
send "uptime"
wait_for_output ", reset reason 1" 2
quit_app

after="$(snapshot)"
[ "$before" = "$after" ] || fail "files in the project changed, were added or were removed" \
  "$(diff <(printf '%s\n' "$before") <(printf '%s\n' "$after") | head -20 || true)"

echo "PASS: nyx"

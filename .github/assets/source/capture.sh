#!/usr/bin/env bash
# Runs examples/demo on both stock boards, headless and with the clock held, and saves the frames
# and terminal screens the README's images are built from. Run render.sh afterwards. Everything
# works on the copies tests/integration/demo.sh keeps in build/, with the internet forced
# offline, so that no capture depends on a public site.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
scratch="$(mktemp -d)"
app_pid=""
trap '[ -z "$app_pid" ] || kill -9 "$app_pid" 2>/dev/null || true; rm -rf "$scratch"' EXIT

go build -C "$root" -o "$scratch/preflash" ./cmd/preflash
pf() { "$scratch/preflash" "$@"; }

use_project() { # use_project <folder>
  project="$root/build/integration/demo/$1/demo"
  mkdir -p "$project"
  rsync -a --delete "$root/examples/demo/" "$project/"
  local sum
  sum="$(printf '%s' "$(cd "$project" && pwd -P)" | shasum -a 256 | cut -c1-8)"
  rm -rf "$HOME/.preflash/state/demo-$sum"
}

seen() { # seen <text> <times>
  until [ "$(grep -cF "$1" "$scratch/app.log" || true)" -ge "$2" ]; do
    kill -0 "$app_pid" || { cat "$scratch/app.log" >&2; exit 1; }
    sleep 0.2
  done
}

# Boots twice, because only the second boot starts with the internet off.
boot() { # boot [run flags]
  pf run --project "$project" --headless "$@" < /dev/null > "$scratch/app.log" 2>&1 &
  app_pid=$!
  seen "ready on" 1
  pf hardware --project "$project" internet offline > /dev/null
  pf send --project "$project" restart
  seen "ready on" 2
  seen "ESP_ERR_HTTP_CONNECT" 1
  sleep 1
  pf send --project "$project" freeze 2026-07-14T09:41:00Z
}

# Saves the panel once two looks in a row match.
frame() { # frame <name>
  local now="$scratch/now.png" last="$scratch/last.png"
  pf shot --project "$project" "$last"
  sleep 0.5
  pf shot --project "$project" "$now"
  until cmp -s "$now" "$last"; do
    cp "$now" "$last"
    sleep 0.5
    pf shot --project "$project" "$now"
  done
  cp "$now" "$here/$1.png"
  echo "wrote $1.png"
}

stop() {
  pf quit --project "$project"
  wait "$app_pid" || true
  app_pid=""
}

page() { pf send --project "$project" page "$1"; }
force() { pf hardware --project "$project" "$@" > /dev/null; }

# ---- ESP32-S3-Touch-LCD-4B

use_project 4b
go run -C "$root" ./.github/assets/source/tuishot "$scratch/preflash" "$project" "$here"

use_project 4b
boot
page faults
force i2c pcf85063
frame 4b-faults
force i2c healthy
page power
force battery 15
frame 4b-power
force battery none
page home
force rotation 90
frame 4b-turned
stop

# ---- ESP32-S3-Touch-LCD-3.49-V2

use_project 349
boot --board ESP32-S3-Touch-LCD-3.49-V2
sleep 1
frame 349-home
page faults
force i2c qmi8658
frame 349-faults
force i2c healthy
page network
frame 349-network
stop

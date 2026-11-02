#!/usr/bin/env bash
# Runs the terminal app for real, in a pseudo-terminal, on a project with a .preflashrc.
# LVGL comes from the test suite's download, so build that first.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

lvgl="$root/build/tests/_deps/lvgl-src"
[ -f "$lvgl/lvgl.h" ] || { echo "FAIL: no LVGL in $lvgl: build tests/runtime first" >&2; exit 1; }

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

# Kept between runs, so that only what changed gets rebuilt.
project="$root/build/integration/tui/tui_fixture"
mkdir -p "$project/managed_components" "$project/sim"
rsync -a "$root/tests/fixtures/lvgl_project/" "$project/"
rsync -a --exclude /demos --exclude /docs --exclude /examples --exclude /tests \
  "$lvgl/" "$project/managed_components/lvgl__lvgl/"
# The tests edit this file, and rsync puts back its old time, which a build would believe.
touch "$project/main/main.cpp"
cp "$root/tests/fixtures/snoozel-4b.yaml" "$project/sim/"
sed -i '' 's/^name: .*/name: mod/' "$project/sim/snoozel-4b.yaml"
cat > "$project/.preflashrc" <<'RC'
console:
  prompt: "fixture>"

shortcuts:
  - label: Ping
    send: ping
  - label: Two
    send:
      - brightness 40
      - ping
  - label: Rotate
    send: rotate {degrees}
    ask:
      degrees: 90
  - label: Restart
    send: restart
  - label: Flood
    send: flood
  - label: Unsupported
    send: unsupported

status:
  - name: last
    pattern: 'fixture: (\w+)'
  - name: backlight
  - name: rotation
  - name: wifi
RC

go build -C "$root" -o "$scratch/preflash" ./cmd/preflash

# Stand-ins, so that the crash screen's keys attach to nothing, edit nothing and leave the
# Mac's clipboard alone.
tools="$scratch/tools"
mkdir "$tools"
for tool in lldb editor; do
  printf '#!/bin/sh\necho "$@" > "%s/%s.args"\n' "$tools" "$tool" > "$tools/$tool"
done
printf '#!/bin/sh\ncat > "%s/copied"\n' "$tools" > "$tools/pbcopy"
chmod +x "$tools"/*
export PREFLASH_LLDB="$tools/lldb" EDITOR="$tools/editor" PREFLASH_PBCOPY="$tools/pbcopy"
export PREFLASH_E2E_TOOLS="$tools"

# Go caches a passing test by its inputs, and those include neither the binary's source nor
# the fixture, so -count=1.
PREFLASH_E2E_BINARY="$scratch/preflash" PREFLASH_E2E_PROJECT="$project" \
  go test -C "$root" -count=1 -timeout 30m "$@" ./tests/e2e

echo "PASS: tui"

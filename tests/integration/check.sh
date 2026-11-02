#!/usr/bin/env bash
# Replays captures against the real chip models. The hand-written captures test the comparison,
# and the capture firmware runs under Preflash on each stock board, so the firmware's list, its
# output and preflash check are known to fit together. None of this has met a real chip, and every
# capture here says so.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$root/tests/integration/app.sh"

scratch="$(mktemp -d)"
app_pid=""
trap '[ -z "$app_pid" ] || kill -9 "$app_pid" 2>/dev/null; rm -rf "$scratch"' EXIT

fail() {
  echo "FAIL: $1" >&2
  [ -n "${2:-}" ] && printf '%s\n' "$2" >&2
  exit 1
}

go build -C "$root" -o "$scratch/preflash" ./cmd/preflash
pf() { "$scratch/preflash" "$@"; }
captures="$root/tests/fixtures/captures"
four_b="ESP32-S3-Touch-LCD-4B"
three_49="ESP32-S3-Touch-LCD-3.49-V2"

says_synthetic() {
  local output="$1"
  [ "$(printf '%s\n' "$output" | grep -c 'SYNTHETIC')" -eq 2 ] || fail "no synthetic warning at each end" "$output"
  printf '%s\n' "$output" | tail -1 | grep -qF 'not evidence about any real chip' ||
    fail "the last line doesn't say it's no evidence" "$output"
}

# Hand-written captures that agree with the models.
for pair in "agrees-4b:$four_b" "agrees-349:$three_49"; do
  output="$(pf check "$captures/${pair%%:*}.jsonl" --board "${pair#*:}")" || fail "${pair%%:*} should pass" "$output"
  says_synthetic "$output"
done

# One that doesn't, in every way a difference can show.
if output="$(pf check "$captures/differs-4b.jsonl" --board "$four_b" 2>&1)"; then
  fail "a capture with mismatches passed" "$output"
fi
says_synthetic "$(printf '%s\n' "$output" | grep -v '^preflash: ')"
for want in \
  '#1 register 0x03: real 0xf0, model 0xff: MISMATCH' \
  '#2 register 0xfd: real 0x83, model 0x00: MISMATCH' \
  '#4 register 0x814e: real 0x80, model 0x00: expected (sensor noise)' \
  'expected (power state)' \
  'expected (time)' \
  '#7 register 0x35: real 0x12, model 0x00: expected (sensor noise)' \
  '#8 register 0x7f: real ESP_OK, model refused: qmi8658 register 0x7f' \
  'preflash: the capture and the models disagree'; do
  printf '%s\n' "$output" | grep -qF -- "$want" || fail "no line with: $want" "$output"
done

# A capture of the wrong board goes nowhere near the models.
if output="$(pf check "$captures/agrees-4b.jsonl" --board "$three_49" 2>&1)"; then
  fail "a 4B capture passed as a 3.49" "$output"
fi
printf '%s\n' "$output" | grep -qF "this capture is of $four_b, not $three_49" || fail "no word of the wrong board" "$output"

# A board that extends a stock one takes that board's captures.
cat > "$scratch/modded.yaml" <<YAML
name: modded
extends: $four_b
changes:
  backlight:
    enable: gpio43
YAML
pf check "$captures/agrees-4b.jsonl" --board "$scratch/modded.yaml" > /dev/null || fail "a modded board refused its stock capture"

# The firmware itself, on each board. Kept between runs, so that only what changed gets rebuilt.
run_capture() {
  local board="$1" defaults="$2" project="$root/build/integration/check/$2"
  mkdir -p "$project"
  rsync -a --delete --exclude /sdkconfig "$root/hardware/capture/" "$project/"
  cat "$project/sdkconfig.defaults" "$project/sdkconfig.defaults.$defaults" > "$project/sdkconfig"

  # The first build can take longer than a test's wait for output.
  pf run --project "$project" --board "$board" --headless <<< "@sim quit" || fail "the capture firmware didn't build on $board"
  start_app pf run --project "$project" --board "$board" --headless
  wait_for_output "Press Enter"
  send go
  wait_for_output '{"type":"end"'

  grep '^{' "$app_log" > "$scratch/$defaults.jsonl"
  head -1 "$scratch/$defaults.jsonl" | grep -qF '"source":"synthetic"' ||
    fail "a run under Preflash didn't say it was synthetic" "$(head -1 "$scratch/$defaults.jsonl")"
  output="$(pf check "$scratch/$defaults.jsonl" --board "$board")" || fail "the firmware's own run failed on $board" "$output"
  says_synthetic "$output"
  printf '%s\n' "$output" | grep -qF ' 0 mismatched' || fail "mismatches on $board" "$output"
  ! printf '%s\n' "$output" | grep -qF 'no transactions' || fail "a chip of $board went uncaptured" "$output"

  send "@sim quit"
  finish_app
  app_pid=""
}

run_capture "$four_b" 4b
run_capture "$three_49" 349

echo "check: OK"

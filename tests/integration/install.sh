#!/usr/bin/env bash
# Packages a release, installs it with install.sh from a local folder, and builds and runs
# a project with the installed binary, away from this checkout, to prove the release finds
# its own runtime.
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

"$root/scripts/package.sh" 9.9.9 "$scratch/release" >/dev/null

prefix="$scratch/prefix"
log="$(PREFLASH_PREFIX="$prefix" PREFLASH_DOWNLOAD_URL="file://$scratch/release" sh "$root/install.sh" 2>&1)" ||
  fail "install.sh failed" "$log"
case "$log" in *"installed 9.9.9"*) ;; *) fail "install.sh didn't report the version" "$log" ;; esac
[ "$("$prefix/bin/preflash" version)" = "preflash 9.9.9" ] || fail "the installed binary has the wrong version"

# A second install replaces the first, and a bad checksum leaves it alone.
PREFLASH_PREFIX="$prefix" PREFLASH_DOWNLOAD_URL="file://$scratch/release" sh "$root/install.sh" >/dev/null 2>&1 ||
  fail "installing over an install failed"
echo "0000  preflash-darwin-arm64.tar.gz" > "$scratch/release/preflash-darwin-arm64.tar.gz.sha256"
if log="$(PREFLASH_PREFIX="$prefix" PREFLASH_DOWNLOAD_URL="file://$scratch/release" sh "$root/install.sh" 2>&1)"; then
  fail "install.sh accepted a bad checksum" "$log"
fi
[ -f "$prefix/share/preflash/runtime/idf/tools/cmake/project.cmake" ] || fail "a failed install removed the runtime"

# Run from elsewhere with a fresh home, so neither this checkout nor its builds can help.
project="$scratch/project"
cp -R "$root/tests/fixtures/mini_project" "$project"
mkdir "$scratch/home"
cd "$scratch"
start_app env -u PREFLASH_ROOT HOME="$scratch/home" "$prefix/bin/preflash" run --headless --project "$project"
for _ in $(seq 1 1200); do
  grep -qF "app_main finished" "$app_log" && break
  kill -0 "$app_pid" 2>/dev/null || fail "the installed app stopped" "$(cat "$app_log")"
  sleep 0.1
done
grep -qF "hello from app_main" "$app_log" || fail "the installed app never ran" "$(cat "$app_log")"
quit_app

echo "install: ok"

#!/usr/bin/env bash
# Builds the unmodified fixture project against Preflash's own IDF_PATH, the way
# a real project would be built, from a folder whose path contains a space.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export IDF_PATH="$root/runtime/idf"

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
work="$scratch/a folder"
mkdir "$work"

fail() {
  echo "FAIL: $1" >&2
  [ -n "${2:-}" ] && printf '%s\n' "$2" >&2
  exit 1
}

fresh_copy() {
  rm -rf "$work/project" "$work/build"
  cp -R "$root/tests/fixtures/mini_project" "$work/project"
}

configure() {
  cmake -S "$work/project" -B "$work/build" 2>&1
}

listing() {
  (cd "$work/project" && find . -type f -print0 | sort -z | xargs -0 shasum)
}

expect_output() {
  case "$output" in
    *"$1"*) ;;
    *) fail "app output lacks: $1" "$output" ;;
  esac
}

expect_no_output() {
  case "$output" in
    *"$1"*) fail "app output has: $1" "$output" ;;
  esac
}

expect_stale_project() {
  if log="$(configure)"; then
    fail "$1: configure succeeded" "$log"
  fi
  case "$log" in
    *"run idf.py reconfigure"*) ;;
    *) fail "$1: no reconfigure hint" "$log" ;;
  esac
}

# Builds and runs the unmodified project.
fresh_copy
before="$(listing)"
configure_log="$(configure)" || fail "configure" "$configure_log"
build_log="$(cmake --build "$work/build" 2>&1)" || fail "build" "$build_log"
case "$build_log" in
  *warning*) fail "the build printed a warning" "$build_log" ;;
esac
output="$("$work/build/app")"
expect_output "hello from app_main"
expect_output "CONFIG_THING_VALUE=7"
expect_output "CONFIG_THING_ENABLED=1"
expect_output "CONFIG_THING_MASK=0x10"
expect_output "CONFIG_IDF_TARGET=esp32s3"
expect_no_output "CONFIG_THING_DISABLED"
expect_output "thing=14"
expect_output "constructor ran"
expect_output "embedded=hello bytes"
expect_output "embedded text=notes length=6"
expect_output "lvgl os_none=1 color_depth=16 sdl=0"
expect_output "ESP_PLATFORM is defined"
expect_no_output "asserts are off"

# A change to an embedded file reaches the next build. The wait is for make,
# which compares timestamps to the second.
sleep 1
printf 'changed bytes' > "$work/project/assets/hello.txt"
build_log="$(cmake --build "$work/build" 2>&1)" || fail "rebuild" "$build_log"
output="$("$work/build/app")"
expect_output "embedded=changed bytes"
printf 'hello bytes' > "$work/project/assets/hello.txt"

# Nothing is written inside the project.
after="$(listing)"
[ "$before" = "$after" ] || fail "the project folder changed" "$(diff <(echo "$before") <(echo "$after") || true)"

# ESP-IDF's project.cmake finds project components in components/ by itself.
fresh_copy
sed -i '' '/EXTRA_COMPONENT_DIRS/d' "$work/project/CMakeLists.txt"
configure_log="$(configure)" || fail "configure without EXTRA_COMPONENT_DIRS" "$configure_log"

# EXTRA_COMPONENT_DIRS can name a component's own folder.
fresh_copy
sed -i '' 's|set(EXTRA_COMPONENT_DIRS components)|set(EXTRA_COMPONENT_DIRS components/thing)|' "$work/project/CMakeLists.txt"
configure_log="$(configure)" || fail "configure with a component folder" "$configure_log"

# EXTRA_COMPONENT_DIRS naming a missing folder stops the configure.
fresh_copy
sed -i '' 's|set(EXTRA_COMPONENT_DIRS components)|set(EXTRA_COMPONENT_DIRS nowhere)|' "$work/project/CMakeLists.txt"
if log="$(configure)"; then
  fail "a missing EXTRA_COMPONENT_DIRS folder was accepted" "$log"
fi
case "$log" in
  *nowhere*) ;;
  *) fail "the error doesn't name the missing folder" "$log" ;;
esac

# Preflash never creates sdkconfig itself, since that would write into the project.
fresh_copy
rm "$work/project/sdkconfig"
expect_stale_project "no sdkconfig"

# The same goes for managed components the manifest lists.
fresh_copy
rm -rf "$work/project/managed_components"
expect_stale_project "no managed_components"

# A manifest with nothing to download needs no managed_components folder.
fresh_copy
rm -rf "$work/project/managed_components"
cat > "$work/project/main/idf_component.yml" <<'YML'
dependencies:
  idf: ">=5.4"
  local_thing:
    path: ../components/thing
YML
configure_log="$(configure)" || fail "configure with only local dependencies" "$configure_log"

echo "PASS: build_fixture"

#!/usr/bin/env bash
# Builds a release archive: bin/preflash, with the runtime it builds firmware against in
# share/preflash. Usage: scripts/package.sh <version> <output folder>
set -euo pipefail

version="${1:?usage: scripts/package.sh <version> <output folder>}"
out="${2:?usage: scripts/package.sh <version> <output folder>}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

[ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ] || { echo "package.sh: build this on an Apple Silicon Mac" >&2; exit 1; }

name="preflash-darwin-arm64"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/$name/bin" "$stage/$name/share/preflash" "$out"

go build -C "$root" -trimpath -ldflags "-s -w -X main.version=$version" -o "$stage/$name/bin/preflash" ./cmd/preflash
# Only the files git tracks, so a local build folder can't end up in a release, read from
# the same tree the binary came from.
(cd "$root" && git ls-files -z runtime | xargs -0 tar -cf -) | tar -x -C "$stage/$name/share/preflash"

tar -C "$stage" -czf "$out/$name.tar.gz" "$name"
(cd "$out" && shasum -a 256 "$name.tar.gz" > "$name.tar.gz.sha256")
echo "$out/$name.tar.gz"

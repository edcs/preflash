#!/usr/bin/env bash
# Builds a release archive: bin/preflash, with the runtime it builds firmware against in
# share/preflash. Usage: scripts/package.sh <version> <output folder>
set -euo pipefail

version="${1:?usage: scripts/package.sh <version> <output folder>}"
out="${2:?usage: scripts/package.sh <version> <output folder>}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

case "$(uname -s)-$(uname -m)" in
Darwin-arm64) platform="darwin-arm64" ;;
Linux-x86_64) platform="linux-amd64" ;;
*) echo "package.sh: Preflash builds on Apple Silicon Macs and x86_64 Linux" >&2; exit 1 ;;
esac

name="preflash-$platform"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/$name/bin" "$stage/$name/share/preflash" "$out"

go build -C "$root" -trimpath -ldflags "-s -w -X main.version=$version" -o "$stage/$name/bin/preflash" ./cmd/preflash
# Only the files git tracks, so a local build folder can't end up in a release, read from
# the same tree the binary came from.
(cd "$root" && git ls-files -z runtime | xargs -0 tar -cf -) | tar -x -C "$stage/$name/share/preflash"

cp "$root/LICENSE" "$stage/$name/share/preflash/"

tar -C "$stage" -czf "$out/$name.tar.gz" "$name"
if command -v sha256sum >/dev/null; then
	(cd "$out" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")
else
	(cd "$out" && shasum -a 256 "$name.tar.gz" > "$name.tar.gz.sha256")
fi
echo "$out/$name.tar.gz"

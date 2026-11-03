#!/bin/sh
# Installs Preflash from a GitHub release:
#
#   curl -fsSL https://raw.githubusercontent.com/edcs/preflash/main/install.sh | sh
#
# PREFLASH_VERSION picks a release (default: the latest), and PREFLASH_PREFIX picks where
# it goes (default: ~/.local, so bin/preflash and share/preflash land there).
set -eu

repo="edcs/preflash"
name="preflash-darwin-arm64"

say() { printf 'preflash: %s\n' "$*"; }
die() { printf 'preflash: %s\n' "$*" >&2; exit 1; }

main() {
	[ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ] ||
		die "Preflash runs on Apple Silicon Macs only"
	command -v curl >/dev/null || die "curl is missing"

	prefix="${PREFLASH_PREFIX:-$HOME/.local}"
	version="${PREFLASH_VERSION:-latest}"
	if [ -n "${PREFLASH_DOWNLOAD_URL:-}" ]; then
		base="$PREFLASH_DOWNLOAD_URL"
	elif [ "$version" = latest ]; then
		base="https://github.com/$repo/releases/latest/download"
	else
		base="https://github.com/$repo/releases/download/v${version#v}"
	fi

	work="$(mktemp -d)"
	trap 'rm -rf "$work"' EXIT INT TERM

	say "downloading $name ($version)"
	curl -fsSL -o "$work/$name.tar.gz" "$base/$name.tar.gz" || die "couldn't download $base/$name.tar.gz"
	curl -fsSL -o "$work/$name.tar.gz.sha256" "$base/$name.tar.gz.sha256" || die "couldn't download the checksum"
	(cd "$work" && shasum -a 256 -c "$name.tar.gz.sha256" >/dev/null) || die "the download doesn't match its checksum"
	tar -xzf "$work/$name.tar.gz" -C "$work"

	mkdir -p "$prefix/bin" "$prefix/share"
	# The new runtime goes in beside the old one, so a failed copy leaves the old one whole.
	rm -rf "$prefix/share/preflash.new"
	cp -R "$work/$name/share/preflash" "$prefix/share/preflash.new"
	rm -rf "$prefix/share/preflash"
	mv "$prefix/share/preflash.new" "$prefix/share/preflash"
	cp "$work/$name/bin/preflash" "$prefix/bin/preflash.new"
	chmod 755 "$prefix/bin/preflash.new"
	mv "$prefix/bin/preflash.new" "$prefix/bin/preflash"

	say "installed $("$prefix/bin/preflash" version | cut -d' ' -f2) in $prefix/bin/preflash"

	missing=""
	command -v cmake >/dev/null || missing="$missing cmake"
	command -v python3 >/dev/null || missing="$missing python"
	if ! { command -v pkg-config >/dev/null && pkg-config --exists sdl2; } &&
		! { command -v brew >/dev/null && brew list sdl2 >/dev/null 2>&1; }; then
		missing="$missing sdl2"
	fi
	[ -z "$missing" ] || say "Preflash also needs:$missing. With Homebrew: brew install$missing"

	case ":$PATH:" in
	*":$prefix/bin:"*) ;;
	*) say "$prefix/bin isn't on your PATH. Add it with: export PATH=\"$prefix/bin:\$PATH\"" ;;
	esac
}

main "$@"

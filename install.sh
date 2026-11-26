#!/bin/sh
# Installs Preflash from a GitHub release:
#
#   curl -fsSL https://raw.githubusercontent.com/edcs/preflash/main/install.sh | sh
#
# PREFLASH_VERSION picks a release (default: the latest), and PREFLASH_PREFIX picks where
# it goes (default: ~/.local, so bin/preflash and share/preflash land there).
set -eu

repo="edcs/preflash"
name=""

say() { printf 'preflash: %s\n' "$*"; }
die() { printf 'preflash: %s\n' "$*" >&2; exit 1; }

main() {
	case "$(uname -s)-$(uname -m)" in
	Darwin-arm64) name="preflash-darwin-arm64" ;;
	Linux-x86_64) name="preflash-linux-amd64" ;;
	*) die "Preflash runs on Apple Silicon Macs and x86_64 Linux" ;;
	esac
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
	# Compared by hand: some sha256sum builds only warn about a malformed checksum file.
	if command -v sha256sum >/dev/null; then
		actual="$(sha256sum "$work/$name.tar.gz")"
	else
		actual="$(shasum -a 256 "$work/$name.tar.gz")"
	fi
	expected="$(cut -d' ' -f1 "$work/$name.tar.gz.sha256")"
	[ -n "$expected" ] && [ "${actual%% *}" = "$expected" ] || die "the download doesn't match its checksum"
	tar -xzf "$work/$name.tar.gz" -C "$work"

	mkdir -p "$prefix/bin" "$prefix/share"
	# Both halves are copied before either replaces what's there.
	cp "$work/$name/bin/preflash" "$prefix/bin/preflash.new" || die "couldn't copy preflash to $prefix/bin"
	chmod 755 "$prefix/bin/preflash.new"
	# The new runtime goes in beside the old one, which stays until the new one is in place.
	share="$prefix/share/preflash"
	rm -rf "$share.new" "$share.old"
	cp -R "$work/$name/share/preflash" "$share.new" || die "couldn't copy the runtime to $share.new"
	[ ! -e "$share" ] || mv "$share" "$share.old" || die "couldn't move the old runtime aside"
	if ! mv "$share.new" "$share"; then
		[ ! -e "$share.old" ] || mv "$share.old" "$share"
		die "couldn't put the runtime in $share"
	fi
	rm -rf "$share.old"
	mv "$prefix/bin/preflash.new" "$prefix/bin/preflash"

	say "installed $("$prefix/bin/preflash" version | cut -d' ' -f2) in $prefix/bin/preflash"

	if [ "$(uname -s)" = Linux ]; then
		missing=""
		command -v cmake >/dev/null || missing="$missing, cmake"
		command -v python3 >/dev/null || missing="$missing, python3"
		{ command -v c++ >/dev/null || command -v g++ >/dev/null; } || missing="$missing, a C++ compiler"
		{ command -v pkg-config >/dev/null && pkg-config --exists sdl2 libcurl; } || missing="$missing, the SDL2 and libcurl development files"
		if [ -n "$missing" ]; then
			say "Preflash also needs ${missing#, }."
			distro=""
			[ ! -r /etc/os-release ] || distro="$(. /etc/os-release && echo "${ID:-} ${ID_LIKE:-}")"
			case " $distro " in
			*" fedora "* | *" rhel "*) say "On Fedora: sudo dnf install cmake gcc-c++ SDL2-devel libcurl-devel" ;;
			*" debian "* | *" ubuntu "*) say "On Debian/Ubuntu: sudo apt install cmake g++ libsdl2-dev libcurl4-openssl-dev" ;;
			*) say "With Homebrew: brew install cmake sdl2 curl" ;;
			esac
		fi
	else
		missing=""
		command -v cmake >/dev/null || missing="$missing cmake"
		command -v python3 >/dev/null || missing="$missing python"
		if ! { command -v pkg-config >/dev/null && pkg-config --exists sdl2; } &&
			! { command -v brew >/dev/null && brew list sdl2 >/dev/null 2>&1; }; then
			missing="$missing sdl2"
		fi
		[ -z "$missing" ] || say "Preflash also needs:$missing. With Homebrew: brew install$missing"
	fi

	case ":$PATH:" in
	*":$prefix/bin:"*) ;;
	*) say "$prefix/bin isn't on your PATH. Add it with: export PATH=\"$prefix/bin:\$PATH\"" ;;
	esac
}

main "$@"

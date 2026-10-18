#!/data/data/com.termux/files/usr/bin/bash
# Installs or updates glibct in Termux:
#   curl -fsSL https://raw.githubusercontent.com/ssfuisu/glibct/main/install.sh | bash
# Set GLIBCT_VERSION (for example v1.0.0) to install a given release.

set -euo pipefail

REPO=ssfuisu/glibct
TERMUX_PREFIX=/data/data/com.termux/files/usr
GLIBC_PREFIX=$TERMUX_PREFIX/glibc

say() {
	printf '==> %s\n' "$*"
}

die() {
	printf 'glibct install: %s\n' "$*" >&2
	exit 1
}

# Everything runs inside main, so bash has read the whole script before
# pkg or curl could read from the pipe that feeds it.
main() {
	local version=${GLIBCT_VERSION:-latest} url missing=()

	if [ ! -d "$TERMUX_PREFIX" ] || [ "${PREFIX-}" != "$TERMUX_PREFIX" ]; then
		die "this installer only works in Termux"
	fi
	[ "$(uname -m)" = aarch64 ] ||
		die "glibct supports 64-bit ARM (aarch64) devices only"

	[ -e "$GLIBC_PREFIX/lib/ld-linux-aarch64.so.1" ] || missing+=(glibc)
	[ -x "$GLIBC_PREFIX/bin/bash" ] || missing+=(bash-glibc)
	[ -x "$GLIBC_PREFIX/bin/ls" ] || missing+=(coreutils-glibc)
	if [ ${#missing[@]} -gt 0 ]; then
		if ! dpkg -s glibc-repo >/dev/null 2>&1; then
			say "Adding the Termux glibc package repository"
			pkg install -y glibc-repo </dev/null
			apt update </dev/null
		fi
		say "Installing ${missing[*]}"
		pkg install -y "${missing[@]}" </dev/null
	fi

	if [ "$version" = latest ]; then
		url="https://github.com/$REPO/releases/latest/download"
	else
		url="https://github.com/$REPO/releases/download/$version"
	fi
	# Not local: the EXIT trap runs after main has returned.
	tmp=$(mktemp -d "$TERMUX_PREFIX/tmp/glibct-install.XXXXXX")
	trap 'rm -rf "$tmp"' EXIT

	say "Downloading glibct ($version)"
	curl -fsSL -o "$tmp/glibct-aarch64.tar.gz" "$url/glibct-aarch64.tar.gz"
	curl -fsSL -o "$tmp/SHA256SUMS" "$url/SHA256SUMS"
	(cd "$tmp" && sha256sum -c --quiet SHA256SUMS) ||
		die "the download is damaged (checksum mismatch), please try again"
	tar -xzf "$tmp/glibct-aarch64.tar.gz" -C "$tmp"

	say "Installing glibct"
	install -Dm755 "$tmp/glibct/bin/glibct" "$TERMUX_PREFIX/bin/glibct"
	install -Dm644 "$tmp/glibct/share/bashrc" \
		"$TERMUX_PREFIX/share/glibct/bashrc"
	# Programs that are running keep the old files; new ones get the new
	# ones. Releases before 1.1.0 have no Termux helper library.
	if [ -f "$tmp/glibct/lib/libglibct-termux.so" ]; then
		install -Dm755 "$tmp/glibct/lib/libglibct-termux.so" \
			"$TERMUX_PREFIX/lib/libglibct-termux.so.new"
		mv -f "$TERMUX_PREFIX/lib/libglibct-termux.so.new" \
			"$TERMUX_PREFIX/lib/libglibct-termux.so"
	fi
	install -Dm755 "$tmp/glibct/lib/libglibct.so" \
		"$GLIBC_PREFIX/lib/libglibct.so.new"
	mv -f "$GLIBC_PREFIX/lib/libglibct.so.new" "$GLIBC_PREFIX/lib/libglibct.so"

	# The first run turns the hook on; this also checks that it works.
	"$TERMUX_PREFIX/bin/glibct" true </dev/null ||
		die "glibct is installed but does not start"

	echo
	echo "$("$TERMUX_PREFIX/bin/glibct" --version) is installed."
	echo "Type 'glibct' to start it and 'exit' to leave it."
	echo "Type 'glibct --doctor' if something does not work."
}

main "$@"

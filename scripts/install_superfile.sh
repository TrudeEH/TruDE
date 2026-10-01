#!/bin/sh
set -eu

# Prefer Debian packages if Superfile becomes available in the configured repos.
candidate=$(apt-cache policy superfile 2>/dev/null | awk '/Candidate:/ { print $2 }')
if [ -n "$candidate" ] && [ "$candidate" != '(none)' ]; then
    sudo apt-get install -y -- superfile </dev/tty
    exit 0
fi

# Official release: https://github.com/yorukot/superfile/releases/tag/v1.6.0
# Pin both the version and upstream SHA-256 digests for reproducible installs.
version=1.6.0
arch=$(dpkg --print-architecture)
case $arch in
    amd64) checksum=d45b0e95072629a6aa7983a84eedca9cd7a98861e67ef91be1415496e3dda309 ;;
    arm64) checksum=2e8150dab1a139e3a081bd74a9c775155e8d0241c34ae3f8d30817a3df1b8f06 ;;
    *)
        printf 'Superfile has no official Linux release for %s.\n' "$arch" >&2
        exit 1
        ;;
esac

bin_dir=$HOME/.local/bin
mkdir -p "$bin_dir"
temporary_dir=$(mktemp -d)
staged_binary=
trap 'rm -rf "$temporary_dir"; [ -z "$staged_binary" ] || rm -f "$staged_binary"' EXIT
trap 'exit 1' HUP INT TERM

archive=superfile-linux-v$version-$arch
printf 'Installing Superfile %s for %s (official release).\n' "$version" "$arch"
curl --fail --location --show-error --retry 3 \
    --output "$temporary_dir/$archive.tar.gz" \
    "https://github.com/yorukot/superfile/releases/download/v$version/$archive.tar.gz"
printf '%s  %s\n' "$checksum" "$temporary_dir/$archive.tar.gz" | sha256sum --check --status
tar -xzf "$temporary_dir/$archive.tar.gz" -C "$temporary_dir" "./dist/$archive/spf"

# Stage on the same filesystem, then replace the binary atomically.
staged_binary=$(mktemp "$bin_dir/.spf.XXXXXX")
install -m 0755 "$temporary_dir/dist/$archive/spf" "$staged_binary"
mv -f "$staged_binary" "$bin_dir/spf"
staged_binary=

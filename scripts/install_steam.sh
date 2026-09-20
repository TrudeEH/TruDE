#!/bin/sh
set -eu

backports=/etc/apt/sources.list.d/dotfiles-backports.sources
backports_suite=stable-backports

if [ "$(id -u)" -eq 0 ]; then
    echo "Run this script as your normal desktop user, not root." >&2
    exit 1
fi

. /etc/os-release
debian_major=${VERSION_ID:-}
debian_major=${debian_major%%.*}
case $debian_major in
    ''|*[!0-9]*) debian_major=0 ;;
esac
if [ "${ID:-}" != debian ] || [ "$debian_major" -lt 13 ] || [ -z "${VERSION_CODENAME:-}" ]; then
    echo "This installer supports Debian 13 (trixie) and newer Debian releases." >&2
    exit 1
fi

if ! dpkg --print-foreign-architectures | grep -qx i386; then
    pkexec dpkg --add-architecture i386
fi

if [ ! -f "$backports" ] || ! grep -Fqx "Suites: $backports_suite" "$backports" || ! grep -Fqx "Components: main contrib non-free non-free-firmware" "$backports"; then
    temporary=$(mktemp)
    cat > "$temporary" <<BACKPORTS
Types: deb
URIs: https://deb.debian.org/debian
Suites: $backports_suite
Components: main contrib non-free non-free-firmware
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg
BACKPORTS
    pkexec install -D -m 0644 "$temporary" "$backports"
    rm -f "$temporary"
fi

pkexec apt-get update
pkexec apt-get install -y -t "$backports_suite" steam-installer

echo "Steam and its 32-bit dependencies are installed."

#!/bin/bash
set -euo pipefail

backports=/etc/apt/sources.list.d/dotfiles-backports.sources
backports_suite=stable-backports

if [[ ${EUID} -eq 0 ]]; then
    echo "Run this script as your normal desktop user, not root." >&2
    exit 1
fi

. /etc/os-release
debian_major=${VERSION_ID:-}
debian_major=${debian_major%%.*}
if [[ ${ID:-} != debian || ! ${debian_major:-} =~ ^[0-9]+$ || $debian_major -lt 13 || -z ${VERSION_CODENAME:-} ]]; then
    echo "This installer supports Debian 13 (trixie) and newer Debian releases." >&2
    exit 1
fi

# Steam needs Debian's 32-bit package archive. This is safe to repeat.
if ! dpkg --print-foreign-architectures | grep -qx i386; then
    sudo dpkg --add-architecture i386
fi

# Match the graphics stack used by the Hyprland setup. Do not duplicate the
# source if install.sh has already created it. Refresh it if the source is stale.
if ! sudo test -f "$backports" || ! sudo grep -Fqx "Suites: $backports_suite" "$backports" || ! sudo grep -Fqx "Components: main contrib non-free non-free-firmware" "$backports"; then
    sudo tee "$backports" >/dev/null <<EOF
Types: deb
URIs: https://deb.debian.org/debian
Suites: $backports_suite
Components: main contrib non-free non-free-firmware
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg
EOF
fi

sudo apt-get update

# APT pulls Steam and all required and recommended 32-bit dependencies. The
# backports target keeps Mesa's 32-bit packages matched with a backported stack.
sudo apt-get install -y -t "$backports_suite" steam-installer

echo "Steam and its 32-bit dependencies are installed."

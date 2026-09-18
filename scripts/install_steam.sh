#!/bin/bash
set -euo pipefail

backports=/etc/apt/sources.list.d/dotfiles-backports.sources

if [[ ${EUID} -eq 0 ]]; then
    echo "Run this script as your normal desktop user, not root." >&2
    exit 1
fi

. /etc/os-release
if [[ ${ID:-} != debian || ${VERSION_CODENAME:-} != trixie ]]; then
    echo "This installer supports Debian 13 (trixie)." >&2
    exit 1
fi

# Steam needs Debian's 32-bit package archive. This is safe to repeat.
if ! dpkg --print-foreign-architectures | grep -qx i386; then
    sudo dpkg --add-architecture i386
fi

# Match the graphics stack used by the Hyprland setup. Do not duplicate the
# source if install.sh has already created it.
if ! sudo test -f "$backports"; then
    sudo tee "$backports" >/dev/null <<'EOF'
Types: deb
URIs: https://deb.debian.org/debian
Suites: trixie-backports
Components: main
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg
EOF
fi

sudo apt-get update

# APT pulls Steam and all required and recommended 32-bit dependencies. The
# backports target keeps Mesa's 32-bit packages matched with a backported stack.
sudo apt-get install -y -t trixie-backports steam-installer

echo "Steam and its 32-bit dependencies are installed."

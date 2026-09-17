#!/bin/bash
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
config_dir=${XDG_CONFIG_HOME:-"$HOME/.config"}
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

# This file is managed by this script. Its presence makes repeated runs safe.
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

# APT installs required and recommended dependencies. Keep every requested
# desktop component in this one transaction so backported PipeWire stays matched.
sudo apt-get install -y -t trixie-backports \
    hyprland hyprland-guiutils quickshell xdg-desktop-portal-hyprland \
    foot fuzzel thunar pipewire-audio wireplumber \
    xdg-desktop-portal-gtk brightnessctl brightness-udev playerctl \
    gvfs thunar-volman tumbler udisks2 \
    qt6-wayland adwaita-qt adwaita-qt6 grim slurp hyprpolkitagent

link_config() {
    local source=$1 target=$2

    mkdir -p "$(dirname "$target")"
    if [[ -L $target && $(readlink -f "$target") == "$source" ]]; then
        return
    fi
    if [[ -e $target || -L $target ]]; then
        mv "$target" "$target.backup-$(date +%Y%m%d-%H%M%S)"
    fi
    ln -s "$source" "$target"
}

link_config "$repo_dir/hypr/hyprland.lua" "$config_dir/hypr/hyprland.lua"
link_config "$repo_dir/scripts/screenshot" "$HOME/.local/bin/dotfiles-screenshot"
for quickshell_file in shell.qml Bar.qml LauncherButton.qml Shortcuts.qml Theme.qml qmldir; do
    link_config "$repo_dir/quickshell/$quickshell_file" "$config_dir/quickshell/$quickshell_file"
done
link_config "$repo_dir/gtk/settings.ini" "$config_dir/gtk-3.0/settings.ini"
link_config "$repo_dir/gtk/settings.ini" "$config_dir/gtk-4.0/settings.ini"

echo "Done. Log out and back in to start Quickshell with Wayland support."

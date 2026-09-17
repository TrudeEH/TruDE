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

# Install the compositor, shell, and their related portal components
# from backports. The -t flag is kept on this focused transaction only.
sudo apt-get install -y -t trixie-backports \
    hyprland hyprland-guiutils quickshell xdg-desktop-portal-hyprland

# The rest of the desktop uses Debian Trixie's normal package priorities.
sudo apt-get install -y \
    foot nautilus gnome-software gnome-software-plugin-flatpak \
    flatpak gnome-text-editor gnome-calculator \
    gnome-disk-utility gnome-keyring pipewire-audio wireplumber \
    network-manager lightdm slick-greeter \
    xdg-desktop-portal-gtk brightnessctl brightness-udev playerctl \
    gvfs udisks2 \
    qt6-wayland adwaita-qt adwaita-qt6 grim slurp wl-clipboard hyprpolkitagent

flatpak --user remote-add --if-not-exists flathub \
    https://dl.flathub.org/repo/flathub.flatpakrepo

# Configure LightDM's Slick Greeter without changing desktop GTK settings.
sudo install -D -m 0644 "$repo_dir/lightdm/slick-greeter.conf" \
    /etc/lightdm/slick-greeter.conf
sudo install -D -m 0644 -o lightdm -g lightdm "$repo_dir/lightdm/gtk.css" \
    /var/lib/lightdm/.config/gtk-3.0/gtk.css

# Let LightDM pass the login password to GNOME Keyring. Appending these lines
# is safe on repeated runs and preserves Debian's PAM configuration.
pam_file=/etc/pam.d/lightdm
if ! sudo grep -Fqx "auth optional pam_gnome_keyring.so" "$pam_file"; then
    echo "auth optional pam_gnome_keyring.so" | sudo tee -a "$pam_file" >/dev/null
fi
if ! sudo grep -Fqx "session optional pam_gnome_keyring.so auto_start" "$pam_file"; then
    echo "session optional pam_gnome_keyring.so auto_start" | sudo tee -a "$pam_file" >/dev/null
fi

sudo systemctl enable lightdm.service

# Clear theme overrides left by previous versions, then request dark mode.
# These settings are harmless to repeat and are skipped outside a user bus.
if command -v gsettings >/dev/null 2>&1 && [[ -n ${DBUS_SESSION_BUS_ADDRESS:-} ]]; then
    gsettings reset org.gnome.desktop.interface gtk-theme || true
    gsettings reset org.gnome.desktop.interface icon-theme || true
    gsettings reset org.gnome.desktop.interface accent-color || true
    gsettings set org.gnome.desktop.interface color-scheme prefer-dark || true
fi

# ======= LINK CONFIG =======
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

remove_obsolete_link() {
    local target=$1 source=$2

    if [[ -L $target && $(readlink "$target") == "$source" ]]; then
        rm "$target"
    fi
}

link_config "$repo_dir/hypr/hyprland.lua" "$config_dir/hypr/hyprland.lua"
link_config "$repo_dir/scripts/screenshot" "$HOME/.local/bin/dotfiles-screenshot"
remove_obsolete_link "$HOME/.local/bin/dotfiles-launcher" "$repo_dir/scripts/launcher"
remove_obsolete_link "$HOME/.local/bin/dotfiles-quickshell" "$repo_dir/scripts/quickshell"
for quickshell_file in shell.qml Bar.qml LauncherButton.qml Launcher.qml NetworkButton.qml NetworkPanel.qml Shortcuts.qml Theme.qml qmldir; do
    link_config "$repo_dir/quickshell/$quickshell_file" "$config_dir/quickshell/$quickshell_file"
done
link_config "$repo_dir/gtk/settings.ini" "$config_dir/gtk-3.0/settings.ini"

echo "Done. Log out and back in to start Quickshell with Wayland support."

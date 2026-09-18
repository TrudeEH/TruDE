#!/bin/bash
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
config_dir=${XDG_CONFIG_HOME:-"$HOME/.config"}
backports=/etc/apt/sources.list.d/dotfiles-backports.sources

check_platform() {
    if [[ ${EUID} -eq 0 ]]; then
        echo "Run this script as your normal desktop user, not root." >&2
        exit 1
    fi

    . /etc/os-release
    if [[ ${ID:-} != debian || ${VERSION_CODENAME:-} != trixie ]]; then
        echo "This installer supports Debian 13 (trixie)." >&2
        exit 1
    fi
}

configure_backports() {
    # This file is managed by this script. Its presence makes repeated runs safe.
    if ! sudo test -f "$backports"; then
        sudo tee "$backports" >/dev/null <<'BACKPORTS'
Types: deb
URIs: https://deb.debian.org/debian
Suites: trixie-backports
Components: main
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg
BACKPORTS
    fi
}

install_packages() {
    sudo apt-get update

    # Install the compositor, shell, and their related portal components
    # from backports. The -t flag is kept on this focused transaction only.
    sudo apt-get install -y -t trixie-backports \
        hyprland hyprland-guiutils quickshell uwsm xdg-desktop-portal-hyprland

    # The rest of the desktop uses Debian Trixie's normal package priorities.
    sudo apt-get install -y \
        curl \
        foot nautilus gnome-software gnome-software-plugin-flatpak \
        flatpak gnome-text-editor gnome-calculator \
        gnome-disk-utility gnome-keyring pipewire-audio wireplumber \
        network-manager lightdm slick-greeter \
        xdg-desktop-portal-gtk brightnessctl brightness-udev playerctl \
        gvfs udisks2 \
        qt6-wayland adwaita-qt adwaita-qt6 grim slurp wl-clipboard swaybg hyprpolkitagent
}

install_font() {
    local font_dir="$HOME/.local/share/fonts/JetBrainsMonoNerdFont"

    if [[ $(fc-match "JetBrainsMono Nerd Font" -f '%{family}') != *"JetBrainsMono Nerd Font"* ]]; then
        mkdir -p "$font_dir"
        local font_archive
        font_archive=$(mktemp)
        curl --fail --location --output "$font_archive" \
            https://github.com/ryanoasis/nerd-fonts/releases/latest/download/JetBrainsMono.tar.xz
        tar -xJf "$font_archive" -C "$font_dir"
        rm -f "$font_archive"
        fc-cache -f "$font_dir"
    fi
}

configure_flatpak() {
    flatpak --user remote-add --if-not-exists flathub \
        https://dl.flathub.org/repo/flathub.flatpakrepo
}

configure_lightdm() {
    # Configure LightDM's Slick Greeter without changing desktop GTK settings.
    sudo install -D -m 0644 "$repo_dir/configs/lightdm/lightdm.conf" \
        /etc/lightdm/lightdm.conf.d/50-dotfiles.conf
    sudo install -D -m 0644 "$repo_dir/configs/lightdm/slick-greeter.conf" \
        /etc/lightdm/slick-greeter.conf
    sudo install -D -m 0644 -o lightdm -g lightdm "$repo_dir/configs/lightdm/gtk.css" \
        /var/lib/lightdm/.config/gtk-3.0/gtk.css
    sudo systemctl enable lightdm.service
}

configure_networking() {
    # Keep NetworkManager as the only manager for network interfaces. Debian's
    # main configuration is loaded after conf.d, so update the authoritative
    # ifupdown setting instead of relying on a lower-priority drop-in.
    local networkmanager_config=/etc/NetworkManager/NetworkManager.conf
    if sudo test -f "$networkmanager_config"; then
        if ! sudo awk '
            /^\[ifupdown\]$/ { in_section=1; next }
            /^\[/ { in_section=0 }
            in_section && /^managed=true$/ { found=1 }
            END { exit !found }
        ' "$networkmanager_config"; then
            sudo cp -a "$networkmanager_config" "$networkmanager_config.backup-$(date +%Y%m%d-%H%M%S)"
            if sudo grep -q '^\[ifupdown\]$' "$networkmanager_config"; then
                sudo sed -i '/^\[ifupdown\]$/,/^\[/{s/^managed=.*/managed=true/}' \
                    "$networkmanager_config"
            else
                printf '\n[ifupdown]\nmanaged=true\n' | sudo tee -a "$networkmanager_config" >/dev/null
            fi
        fi
    fi

    # Do not let ifupdown start dhcpcd for Ethernet at the next boot. Preserve a
    # timestamped backup before removing non-loopback interface stanzas.
    local interfaces_file=/etc/network/interfaces
    if sudo test -f "$interfaces_file" && sudo awk '$1 == "iface" && $2 != "lo" { found=1 } END { exit !found }' "$interfaces_file"; then
        sudo cp -a "$interfaces_file" "$interfaces_file.backup-$(date +%Y%m%d-%H%M%S)"
        sudo awk '
            $1 == "auto" || $1 == "allow-hotplug" { if ($2 != "lo") next }
            $1 == "iface" { skip = ($2 != "lo") }
            skip { next }
            { print }
        ' "$interfaces_file" | sudo tee "$interfaces_file.tmp" >/dev/null
        sudo mv "$interfaces_file.tmp" "$interfaces_file"
    fi
}

configure_pam() {
    # Let LightDM pass the login password to GNOME Keyring. Appending these lines
    # is safe on repeated runs and preserves Debian's PAM configuration.
    local pam_file=/etc/pam.d/lightdm
    if ! sudo grep -Fqx "auth optional pam_gnome_keyring.so" "$pam_file"; then
        echo "auth optional pam_gnome_keyring.so" | sudo tee -a "$pam_file" >/dev/null
    fi
    if ! sudo grep -Fqx "session optional pam_gnome_keyring.so auto_start" "$pam_file"; then
        echo "session optional pam_gnome_keyring.so auto_start" | sudo tee -a "$pam_file" >/dev/null
    fi
}

configure_session() {
    # Use Hyprland's UWSM-managed session for this account. It supplies the
    # Wayland session environment and avoids the raw start-hyprland watchdog.
    cat > "$HOME/.dmrc" <<'DMRC'
[Desktop]
Session=hyprland-uwsm
DMRC
    chmod 644 "$HOME/.dmrc"
}

configure_theme() {
    # Clear theme overrides left by previous versions, then request dark mode.
    # These settings are harmless to repeat and are skipped outside a user bus.
    if command -v gsettings >/dev/null 2>&1 && [[ -n ${DBUS_SESSION_BUS_ADDRESS:-} ]]; then
        gsettings set org.gnome.desktop.interface color-scheme prefer-dark || true
    fi
}

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

link_configs() {
    link_config "$repo_dir/configs/hypr/hyprland.lua" "$config_dir/hypr/hyprland.lua"
    link_config "$repo_dir/assets/wallpapers/wallpaper.jpg" "$HOME/.local/share/backgrounds/dotfiles-wallpaper.jpg"
    link_config "$repo_dir/scripts/screenshot" "$HOME/.local/bin/dotfiles-screenshot"
    remove_obsolete_link "$HOME/.local/bin/dotfiles-launcher" "$repo_dir/scripts/launcher"
    remove_obsolete_link "$HOME/.local/bin/dotfiles-quickshell" "$repo_dir/scripts/quickshell"

    local quickshell_file
    for quickshell_file in shell.qml AppText.qml AudioButton.qml AudioPanel.qml Bar.qml LauncherButton.qml Launcher.qml NetworkButton.qml NetworkPanel.qml NotificationCenter.qml PowerButton.qml PowerMenu.qml Shortcuts.qml Theme.qml TrayMenu.qml TrayMenuView.qml qmldir; do
        link_config "$repo_dir/configs/quickshell/$quickshell_file" "$config_dir/quickshell/$quickshell_file"
    done

    link_config "$repo_dir/configs/gtk/settings.ini" "$config_dir/gtk-3.0/settings.ini"
    link_config "$repo_dir/configs/foot/foot.ini" "$config_dir/foot/foot.ini"
    link_config "$repo_dir/configs/bash/bashrc" "$HOME/.bashrc"
}

main() {
    check_platform
    configure_backports
    install_packages
    install_font
    configure_flatpak
    configure_lightdm
    configure_networking
    configure_pam
    configure_session
    configure_theme
    link_configs

    echo "Done. Log out and back in to start Quickshell with Wayland support."
}

main "$@"

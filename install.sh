#!/bin/bash
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
config_dir=${XDG_CONFIG_HOME:-"$HOME/.config"}
backports=/etc/apt/sources.list.d/dotfiles-backports.sources
backports_suite=stable-backports
debian_components_sources=/etc/apt/sources.list.d/dotfiles-components.sources

check_platform() {
    if [[ ${EUID} -eq 0 ]]; then
        echo "Run this script as your normal desktop user, not root." >&2
        exit 1
    fi

    . /etc/os-release
    local debian_major=${VERSION_ID:-}; debian_major=${debian_major%%.*}
    if [[ ${ID:-} != debian || ! ${debian_major:-} =~ ^[0-9]+$ || $debian_major -lt 13 || -z ${VERSION_CODENAME:-} ]]; then
        echo "This installer supports Debian 13 (trixie) and newer Debian releases." >&2
        exit 1
    fi
}

configure_debian_sources() {
    # Provide a complete Debian source definition. Some installations leave
    # the original main entries commented out, so do not rely on them.
    if ! sudo test -f "$debian_components_sources" || ! sudo grep -Fqx "Components: main contrib non-free non-free-firmware" "$debian_components_sources"; then
        sudo tee "$debian_components_sources" >/dev/null <<SOURCES
Types: deb
URIs: https://deb.debian.org/debian
Suites: stable stable-updates
Components: main contrib non-free non-free-firmware
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg

Types: deb
URIs: https://deb.debian.org/debian-security
Suites: stable-security
Components: main contrib non-free non-free-firmware
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg
SOURCES
    fi
}

configure_backports() {
    # This file follows Debian stable, so upgrades do not require a script change.
    if ! sudo test -f "$backports" || ! sudo grep -Fqx "Suites: $backports_suite" "$backports" || ! sudo grep -Fqx "Components: main contrib non-free non-free-firmware" "$backports"; then
        sudo tee "$backports" >/dev/null <<BACKPORTS
Types: deb
URIs: https://deb.debian.org/debian
Suites: $backports_suite
Components: main contrib non-free non-free-firmware
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg
BACKPORTS
    fi
}

install_packages() {
    sudo apt-get update

    # Install the compositor, shell, and their related portal components
    # from backports. libdw1t64 keeps the backported libelf1t64 dependency
    # consistent when stable packages such as bluez are installed afterward.
    sudo apt-get install -y -t "$backports_suite" \
        hyprland hyprland-guiutils hypridle hyprlock libdw1t64 uwsm xdg-desktop-portal-hyprland

    # The rest of the desktop uses the release's normal package priorities.
    sudo apt-get install -y \
        curl \
        foot nautilus gnome-software gnome-software-plugin-flatpak \
        flatpak gnome-text-editor gnome-calculator \
        gnome-disk-utility gnome-keyring pipewire-audio wireplumber \
        network-manager avahi-daemon libnss-mdns lightdm slick-greeter \
        xdg-desktop-portal-gtk brightnessctl brightness-udev playerctl \
        bluez btop pulsemixer whiptail power-profiles-daemon upower \
        cups system-config-printer ipp-usb gvfs udisks2 \
        qt6-wayland adwaita-qt adwaita-qt6 qt6ct grim slurp wl-clipboard swaybg hyprpolkitagent \
        waybar mako-notifier fzf dex jq
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

configure_hardware_services() {
    sudo systemctl enable --now bluetooth.service cups.service power-profiles-daemon.service
}

configure_networking() {
    sudo systemctl enable --now avahi-daemon.service
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

    mkdir -p "$config_dir/qt6ct"
    cat > "$config_dir/qt6ct/qt6ct.conf" <<QT6CT
[Appearance]
color_scheme_path=$config_dir/qt6ct/colors/dotfiles.conf
custom_palette=true
icon_theme=Adwaita
standard_dialogs=default
style=Adwaita-Dark
QT6CT
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
    link_config "$repo_dir/configs/hypr/hypridle.conf" "$config_dir/hypr/hypridle.conf"
    link_config "$repo_dir/configs/hypr/hyprlock.conf" "$config_dir/hypr/hyprlock.conf"
    link_config "$repo_dir/assets/wallpapers/wallpaper.png" "$HOME/.local/share/backgrounds/dotfiles-wallpaper.png"
    link_config "$repo_dir/scripts/screenshot" "$HOME/.local/bin/dotfiles-screenshot"
    link_config "$repo_dir/scripts/quickshell-network-details" "$HOME/.local/bin/dotfiles-network-details"
    link_config "$repo_dir/scripts/network-tui" "$HOME/.local/bin/dotfiles-network-tui"
    link_config "$repo_dir/scripts/bluetooth-tui" "$HOME/.local/bin/dotfiles-bluetooth-tui"
    link_config "$repo_dir/scripts/power-profiles-tui" "$HOME/.local/bin/dotfiles-power-profiles-tui"
    link_config "$repo_dir/scripts/power-menu-tui" "$HOME/.local/bin/dotfiles-power-menu-tui"
    link_config "$repo_dir/scripts/system-monitor-tui" "$HOME/.local/bin/dotfiles-system-monitor-tui"
    link_config "$repo_dir/scripts/maintenance-tui" "$HOME/.local/bin/dotfiles-maintenance-tui"
    link_config "$repo_dir/scripts/app-launcher-tui" "$HOME/.local/bin/dotfiles-app-launcher-tui"
    link_config "$repo_dir/scripts/app-launcher-toggle" "$HOME/.local/bin/dotfiles-app-launcher-toggle"
    link_config "$repo_dir/scripts/notification-tui" "$HOME/.local/bin/dotfiles-notification-tui"
    link_config "$repo_dir/scripts/shortcuts-tui" "$HOME/.local/bin/dotfiles-shortcuts-tui"
    link_config "$repo_dir/scripts/waybar-temperature-status" "$HOME/.local/bin/dotfiles-waybar-temperature-status"
    link_config "$repo_dir/scripts/waybar-notification-status" "$HOME/.local/bin/dotfiles-waybar-notification-status"
    link_config "$repo_dir/scripts/waybar-power-status" "$HOME/.local/bin/dotfiles-waybar-power-status"
    link_config "$repo_dir/scripts/waybar-maintenance-status" "$HOME/.local/bin/dotfiles-waybar-maintenance-status"
    link_config "$repo_dir/configs/waybar/config.jsonc" "$config_dir/waybar/config.jsonc"
    link_config "$repo_dir/configs/waybar/style.css" "$config_dir/waybar/style.css"
    link_config "$repo_dir/configs/mako/config" "$config_dir/mako/config"
    link_config "$repo_dir/configs/btop/themes/dotfiles.theme" "$config_dir/btop/themes/dotfiles.theme"
    link_config "$repo_dir/configs/qt6ct/colors/dotfiles.conf" "$config_dir/qt6ct/colors/dotfiles.conf"
    link_config "$repo_dir/configs/systemd/user/hyprpolkitagent.service.d/theme.conf" "$config_dir/systemd/user/hyprpolkitagent.service.d/theme.conf"
    link_config "$repo_dir/configs/systemd/user/xdg-desktop-portal-hyprland.service.d/theme.conf" "$config_dir/systemd/user/xdg-desktop-portal-hyprland.service.d/theme.conf"
    remove_obsolete_link "$HOME/.local/bin/dotfiles-launcher" "$repo_dir/scripts/launcher"
    remove_obsolete_link "$HOME/.local/bin/dotfiles-quickshell" "$repo_dir/scripts/quickshell"
    remove_obsolete_link "$config_dir/swaync/config.json" "$repo_dir/configs/swaync/config.json"
    remove_obsolete_link "$config_dir/swaync/style.css" "$repo_dir/configs/swaync/style.css"
    remove_obsolete_link "$config_dir/systemd/user/dotfiles-mako.service" "$repo_dir/configs/systemd/user/dotfiles-mako.service"

    link_config "$repo_dir/configs/gtk/settings.ini" "$config_dir/gtk-3.0/settings.ini"
    link_config "$repo_dir/configs/foot/foot.ini" "$config_dir/foot/foot.ini"
    link_config "$repo_dir/configs/bash/bashrc" "$HOME/.bashrc"
}

main() {
    check_platform
    configure_debian_sources
    configure_backports
    install_packages
    install_font
    configure_flatpak
    configure_lightdm
    configure_networking
    configure_hardware_services
    configure_pam
    configure_session
    link_configs
    configure_theme

    if [[ -n ${DBUS_SESSION_BUS_ADDRESS:-} ]]; then
        systemctl --user daemon-reload || true
        systemctl --user disable --now swaync.service || true
        systemctl --user enable --now waybar.service mako.service || true
        systemctl --user try-restart hyprpolkitagent.service xdg-desktop-portal-hyprland.service || true
    fi

    echo "Done. Log out and back in to start the Waybar desktop session."
}

main "$@"

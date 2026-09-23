#!/bin/sh
set -eu

repo_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
config_dir=${XDG_CONFIG_HOME:-"$HOME/.config"}
backports=/etc/apt/sources.list.d/dotfiles-backports.sources
backports_suite=stable-backports
debian_components_sources=/etc/apt/sources.list.d/dotfiles-components.sources

check_platform() {
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
}

write_root_file() {
    root_destination=$1
    root_mode=$2
    root_temporary=$(mktemp)
    cat > "$root_temporary"
    if sudo install -D -m "$root_mode" "$root_temporary" "$root_destination"; then
        root_status=0
    else
        root_status=$?
    fi
    rm -f "$root_temporary"
    return "$root_status"
}

configure_debian_sources() {
    if [ ! -f "$debian_components_sources" ] || ! grep -Fqx "Components: main contrib non-free non-free-firmware" "$debian_components_sources"; then
        write_root_file "$debian_components_sources" 0644 <<'SOURCES'
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
    if [ ! -f "$backports" ] || ! grep -Fqx "Suites: $backports_suite" "$backports" || ! grep -Fqx "Components: main contrib non-free non-free-firmware" "$backports"; then
        write_root_file "$backports" 0644 <<BACKPORTS
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
    sudo apt-get install -y -t "$backports_suite" \
        hyprland hyprland-guiutils hypridle hyprlock uwsm xdg-desktop-portal-hyprland \
        </dev/tty
    sudo apt-get install -y \
        atool curl foot micro nnn imv cmus p7zip-full gnome-software-plugin-flatpak \
        flatpak gnome-disk-utility gnome-keyring pipewire-audio wireplumber network-manager avahi-daemon \
        lightdm slick-greeter brightnessctl \
        playerctl bluez btop lm-sensors pulsemixer whiptail \
        power-profiles-daemon upower cups system-config-printer ipp-usb gvfs \
        udisks2 qt6-wayland adwaita-qt adwaita-qt6 qt6ct grim slurp \
        wl-clipboard swaybg hyprpolkitagent waybar mako-notifier fzf dex jq \
        file fontconfig procps xdg-user-dirs xdg-utils \
        </dev/tty
}

install_font() {
    font_dir=$HOME/.local/share/fonts/JetBrainsMonoNerdFont
    case $(fc-match "JetBrainsMono Nerd Font" -f '%{family}') in
        *"JetBrainsMono Nerd Font"*) return ;;
    esac

    mkdir -p "$font_dir"
    font_archive=$(mktemp)
    if ! curl --fail --location --output "$font_archive" \
        https://github.com/ryanoasis/nerd-fonts/releases/latest/download/JetBrainsMono.tar.xz; then
        rm -f "$font_archive"
        return 1
    fi
    if ! tar -xJf "$font_archive" -C "$font_dir"; then
        rm -f "$font_archive"
        return 1
    fi
    rm -f "$font_archive"
    fc-cache -f "$font_dir"
}

configure_flatpak() {
    flatpak --user remote-add --if-not-exists flathub \
        https://dl.flathub.org/repo/flathub.flatpakrepo
}

configure_lightdm() {
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
    networkmanager_config=/etc/NetworkManager/NetworkManager.conf
    if [ -f "$networkmanager_config" ] && ! awk '
        /^[[:space:]]*\[ifupdown\][[:space:]]*$/ { in_section=1; next }
        /^[[:space:]]*\[/ { in_section=0 }
        in_section && /^[[:space:]]*managed[[:space:]]*=[[:space:]]*true[[:space:]]*$/ { found=1 }
        END { exit !found }
    ' "$networkmanager_config"; then
        sudo cp -a "$networkmanager_config" "$networkmanager_config.backup-$(date +%Y%m%d-%H%M%S-%N)"
        networkmanager_temporary=$(mktemp)
        if grep -q '^[[:space:]]*\[ifupdown\][[:space:]]*$' "$networkmanager_config"; then
            awk '
                function finish_section() {
                    if (in_section && !wrote_managed) print "managed=true"
                }
                /^[[:space:]]*\[ifupdown\][[:space:]]*$/ {
                    finish_section()
                    in_section=1
                    wrote_managed=0
                    print
                    next
                }
                /^[[:space:]]*\[/ {
                    finish_section()
                    in_section=0
                }
                in_section && /^[[:space:]]*managed[[:space:]]*=/ {
                    if (!wrote_managed) print "managed=true"
                    wrote_managed=1
                    next
                }
                { print }
                END { finish_section() }
            ' "$networkmanager_config" > "$networkmanager_temporary"
        else
            cat "$networkmanager_config" > "$networkmanager_temporary"
            printf '\n[ifupdown]\nmanaged=true\n' >> "$networkmanager_temporary"
        fi
        if sudo install -m 0644 "$networkmanager_temporary" "$networkmanager_config"; then
            networkmanager_status=0
        else
            networkmanager_status=$?
        fi
        rm -f "$networkmanager_temporary"
        [ "$networkmanager_status" -eq 0 ] || return "$networkmanager_status"
    fi

    interfaces_file=/etc/network/interfaces
    if [ -f "$interfaces_file" ] && awk '$1 == "iface" && $2 != "lo" { found=1 } END { exit !found }' "$interfaces_file"; then
        sudo cp -a "$interfaces_file" "$interfaces_file.backup-$(date +%Y%m%d-%H%M%S-%N)"
        interfaces_temporary=$(mktemp)
        awk '
            $1 == "auto" || $1 == "allow-hotplug" {
                for (field=2; field <= NF; field++) {
                    if ($field == "lo") {
                        print $1 " lo"
                        break
                    }
                }
                next
            }
            $1 == "iface" { skip = ($2 != "lo") }
            skip { next }
            { print }
        ' "$interfaces_file" > "$interfaces_temporary"
        if sudo install -m 0644 "$interfaces_temporary" "$interfaces_file"; then
            interfaces_status=0
        else
            interfaces_status=$?
        fi
        rm -f "$interfaces_temporary"
        [ "$interfaces_status" -eq 0 ] || return "$interfaces_status"
    fi
}

configure_pam() {
    pam_file=/etc/pam.d/lightdm
    if ! grep -Fqx "auth optional pam_gnome_keyring.so" "$pam_file"; then
        printf '%s\n' "auth optional pam_gnome_keyring.so" | sudo tee -a "$pam_file" >/dev/null
    fi
    if ! grep -Fqx "session optional pam_gnome_keyring.so auto_start" "$pam_file"; then
        printf '%s\n' "session optional pam_gnome_keyring.so auto_start" | sudo tee -a "$pam_file" >/dev/null
    fi
}

configure_session() {
    cat > "$HOME/.dmrc" <<'DMRC'
[Desktop]
Session=hyprland-uwsm
DMRC
    chmod 644 "$HOME/.dmrc"
}

configure_theme() {
    if command -v gsettings >/dev/null 2>&1 && [ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
        gsettings set org.gnome.desktop.interface color-scheme prefer-dark || :
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
    link_source=$1
    link_target=$2
    mkdir -p "$(dirname "$link_target")"
    if [ -L "$link_target" ] && [ "$(readlink -f "$link_target")" = "$link_source" ]; then
        return
    fi
    if [ -e "$link_target" ] || [ -L "$link_target" ]; then
        mv "$link_target" "$link_target.backup-$(date +%Y%m%d-%H%M%S-%N)"
    fi
    ln -s "$link_source" "$link_target"
}

link_configs() {
    link_config "$repo_dir/configs/hypr/hyprland.lua" "$config_dir/hypr/hyprland.lua"
    link_config "$repo_dir/configs/hypr/hypridle.conf" "$config_dir/hypr/hypridle.conf"
    link_config "$repo_dir/configs/hypr/hyprlock.conf" "$config_dir/hypr/hyprlock.conf"
    link_config "$repo_dir/assets/wallpapers/default.jpg" "$HOME/.local/share/backgrounds/dotfiles-wallpaper.jpg"
    link_config "$repo_dir/scripts/hypr/screenshot" "$HOME/.local/bin/dotfiles-screenshot"
    link_config "$repo_dir/scripts/hypr/start-idle" "$HOME/.local/bin/dotfiles-hypridle"
    link_config "$repo_dir/scripts/hypr/start-mako" "$HOME/.local/bin/dotfiles-mako"
    link_config "$repo_dir/scripts/tui/network-tui" "$HOME/.local/bin/dotfiles-network-tui"
    link_config "$repo_dir/scripts/tui/bluetooth-tui" "$HOME/.local/bin/dotfiles-bluetooth-tui"
    link_config "$repo_dir/scripts/tui/power-profiles-tui" "$HOME/.local/bin/dotfiles-power-profiles-tui"
    link_config "$repo_dir/scripts/tui/power-menu-tui" "$HOME/.local/bin/dotfiles-power-menu-tui"
    link_config "$repo_dir/scripts/tui/system-monitor-tui" "$HOME/.local/bin/dotfiles-system-monitor-tui"
    link_config "$repo_dir/scripts/tui/temperature-tui" "$HOME/.local/bin/dotfiles-temperature-tui"
    link_config "$repo_dir/scripts/tui/maintenance-tui" "$HOME/.local/bin/dotfiles-maintenance-tui"
    link_config "$repo_dir/scripts/tui/package-manager-tui" "$HOME/.local/bin/dotfiles-package-manager-tui"
    link_config "$repo_dir/scripts/tui/app-launcher-tui" "$HOME/.local/bin/dotfiles-app-launcher-tui"
    link_config "$repo_dir/scripts/hypr/app-launcher-toggle" "$HOME/.local/bin/dotfiles-app-launcher-toggle"
    link_config "$repo_dir/scripts/tui/notification-tui" "$HOME/.local/bin/dotfiles-notification-tui"
    link_config "$repo_dir/scripts/tui/shortcuts-tui" "$HOME/.local/bin/dotfiles-shortcuts-tui"
    link_config "$repo_dir/scripts/tui/settings-tui" "$HOME/.local/bin/dotfiles-settings-tui"
    link_config "$repo_dir/scripts/tui/file-manager-tui" "$HOME/.local/bin/dotfiles-file-manager-tui"
    link_config "$repo_dir/scripts/tui/icon-picker-tui" "$HOME/.local/bin/dotfiles-icon-picker-tui"
    for desktop_file in "$repo_dir"/configs/applications/*.desktop; do
        link_config "$desktop_file" "$HOME/.local/share/applications/$(basename "$desktop_file")"
    done
    link_config "$repo_dir/scripts/nnn/open" "$HOME/.local/bin/dotfiles-nnn-open"
    link_config "$repo_dir/scripts/waybar/temperature-status" "$HOME/.local/bin/dotfiles-waybar-temperature-status"
    link_config "$repo_dir/scripts/waybar/notification-status" "$HOME/.local/bin/dotfiles-waybar-notification-status"
    link_config "$repo_dir/scripts/waybar/status-indicators" "$HOME/.local/bin/dotfiles-waybar-status-indicators"
    link_config "$repo_dir/scripts/waybar/status-menu" "$HOME/.local/bin/dotfiles-waybar-status-menu"
    link_config "$repo_dir/scripts/waybar/power-status" "$HOME/.local/bin/dotfiles-waybar-power-status"
    link_config "$repo_dir/scripts/waybar/maintenance-status" "$HOME/.local/bin/dotfiles-waybar-maintenance-status"
    link_config "$repo_dir/scripts/waybar/launcher-status" "$HOME/.local/bin/dotfiles-waybar-launcher-status"
    link_config "$repo_dir/scripts/waybar/start" "$HOME/.local/bin/dotfiles-waybar"
    link_config "$repo_dir/configs/waybar/config.jsonc" "$config_dir/waybar/config.jsonc"
    link_config "$repo_dir/configs/waybar/style.css" "$config_dir/waybar/style.css"
    link_config "$repo_dir/configs/mako/config" "$config_dir/mako/config"
    link_config "$repo_dir/configs/systemd/user/waybar.service.d/override.conf" "$config_dir/systemd/user/waybar.service.d/override.conf"
    link_config "$repo_dir/configs/systemd/user/mako.service.d/override.conf" "$config_dir/systemd/user/mako.service.d/override.conf"
    link_config "$repo_dir/configs/btop/themes/dotfiles.theme" "$config_dir/btop/themes/dotfiles.theme"
    link_config "$repo_dir/configs/qt6ct/colors/dotfiles.conf" "$config_dir/qt6ct/colors/dotfiles.conf"
    link_config "$repo_dir/configs/systemd/user/hyprpolkitagent.service.d/theme.conf" "$config_dir/systemd/user/hyprpolkitagent.service.d/theme.conf"
    link_config "$repo_dir/configs/systemd/user/xdg-desktop-portal-hyprland.service.d/theme.conf" "$config_dir/systemd/user/xdg-desktop-portal-hyprland.service.d/theme.conf"
    link_config "$repo_dir/configs/gtk/settings.ini" "$config_dir/gtk-3.0/settings.ini"
    link_config "$repo_dir/configs/mimeapps.list" "$config_dir/mimeapps.list"
    link_config "$repo_dir/configs/foot/foot.ini" "$config_dir/foot/foot.ini"
    link_config "$repo_dir/configs/micro/settings.json" "$config_dir/micro/settings.json"
    link_config "$repo_dir/configs/micro/colorschemes/dotfiles.micro" "$config_dir/micro/colorschemes/dotfiles.micro"
    link_config "$repo_dir/configs/nnn/env" "$config_dir/nnn/env"
    link_config "$repo_dir/configs/nnn/plugins" "$config_dir/nnn/plugins"
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

    if [ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
        systemctl --user daemon-reload || :
        systemctl --user enable --now foot-server.socket waybar.service mako.service || :
        systemctl --user try-restart hyprpolkitagent.service xdg-desktop-portal-hyprland.service || :
    fi

    echo "Done. Log out and back in to start the Waybar desktop session."
}

main "$@"

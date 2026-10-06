#!/bin/sh
set -eu

ui_init() {
    ui_reset=
    ui_accent=
    ui_success_color=
    ui_error_color=
    ui_muted=
    if [ -t 1 ] && [ -z "${NO_COLOR+x}" ] && [ "${TERM:-dumb}" != dumb ]; then
        ui_reset=$(printf '\033[0m')
        ui_accent=$(printf '\033[1;38;5;214m')
        ui_success_color=$(printf '\033[1;38;5;77m')
        ui_error_color=$(printf '\033[1;38;5;203m')
        ui_muted=$(printf '\033[38;5;245m')
    fi
}

ui_banner() {
    printf '\n%s  TruDE%s\n' "$ui_accent" "$ui_reset"
    printf '%s  Debian desktop setup%s\n' "$ui_muted" "$ui_reset"
}

ui_step() {
    printf '\n%s==>%s %s\n' "$ui_accent" "$ui_reset" "$1"
}

ui_success() {
    printf '\n%s✓%s %s\n' "$ui_success_color" "$ui_reset" "$1"
}

ui_error() {
    printf '%sError:%s %s\n' "$ui_error_color" "$ui_reset" "$1" >&2
}

repo_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
config_dir=${XDG_CONFIG_HOME:-"$HOME/.config"}
backports=/etc/apt/sources.list.d/dotfiles-backports.sources
backports_suite=
debian_components_sources=/etc/apt/sources.list.d/dotfiles-components.sources

check_platform() {
    if [ "$(id -u)" -eq 0 ]; then
        ui_error "Run this script as your normal desktop user, not root."
        exit 1
    fi

    . /etc/os-release
    debian_major=${VERSION_ID:-}
    debian_major=${debian_major%%.*}
    case $debian_major in
        ''|*[!0-9]*) debian_major=0 ;;
    esac
    if [ "${ID:-}" != debian ] || [ "$debian_major" -lt 13 ] || [ -z "${VERSION_CODENAME:-}" ]; then
        ui_error "This installer supports Debian 13 (trixie) and newer Debian releases."
        exit 1
    fi
    backports_suite=$VERSION_CODENAME-backports
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
    if [ ! -f "$debian_components_sources" ] \
        || ! grep -Fqx "Suites: $VERSION_CODENAME $VERSION_CODENAME-updates" "$debian_components_sources" \
        || ! grep -Fqx "Suites: $VERSION_CODENAME-security" "$debian_components_sources" \
        || ! grep -Fqx "Components: main contrib non-free non-free-firmware" "$debian_components_sources"; then
        write_root_file "$debian_components_sources" 0644 <<SOURCES
Types: deb
URIs: https://deb.debian.org/debian
Suites: $VERSION_CODENAME $VERSION_CODENAME-updates
Components: main contrib non-free non-free-firmware
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg

Types: deb
URIs: https://deb.debian.org/debian-security
Suites: $VERSION_CODENAME-security
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
    # Retire the idle daemon on systems set up by previous installations.
    if [ "$(dpkg-query -W -f='${Status}' hypridle 2>/dev/null || :)" = 'install ok installed' ]; then
        systemctl --user disable --now hypridle.service 2>/dev/null || :
        pkill -x hypridle 2>/dev/null || :
        sudo apt-get remove -y --no-auto-remove -- hypridle </dev/tty
    fi
    # Prefer stable, but allow required backported dependencies and packages
    # that must match their versions in the same transaction (APT 3 / Debian 13).
    sudo apt-get install -y --allow-downgrades --solver 3.0 --no-strict-pinning \
        "hyprland/$backports_suite" "hyprland-guiutils/$backports_suite" \
        "hyprlock/$backports_suite" \
        "uwsm/$backports_suite" "xdg-desktop-portal-hyprland/$backports_suite" \
        build-essential libcurl4-openssl-dev atool bat ca-certificates curl fdisk foot micro imv cmus lazygit ncdu p7zip-full \
        xz-utils less libglib2.0-bin gsettings-desktop-schemas adwaita-icon-theme pkexec \
        flatpak gnome-keyring pipewire-audio wireplumber network-manager avahi-daemon \
        lightdm slick-greeter brightnessctl \
        playerctl bluez btop lm-sensors pulsemixer whiptail \
        power-profiles-daemon upower cups system-config-printer ipp-usb gvfs \
        udisks2 qt6-wayland adwaita-qt adwaita-qt6 qt6ct grim slurp \
        wl-clipboard swaybg hyprpolkitagent waybar mako-notifier fzf dex jq \
        file fontconfig procps xdg-user-dirs xdg-utils \
        </dev/tty
}

install_superfile() (
    # Keep temporary variables and cleanup traps scoped to this installation.
    set -eu

    # Prefer Debian packages if Superfile becomes available in the configured repos.
    candidate=$(apt-cache policy superfile 2>/dev/null | awk '/Candidate:/ { print $2 }')
    if [ -n "$candidate" ] && [ "$candidate" != '(none)' ]; then
        sudo apt-get install -y -- superfile </dev/tty
        exit 0
    fi

    arch=$(dpkg --print-architecture)
    case $arch in
        amd64|arm64) ;;
        *)
            ui_error "Superfile has no official Linux release for $arch."
            exit 1
            ;;
    esac

    bin_dir=$HOME/.local/bin
    mkdir -p "$bin_dir"
    temporary_dir=$(mktemp -d)
    staged_binary=
    trap 'rm -rf "$temporary_dir"; [ -z "$staged_binary" ] || rm -f "$staged_binary"' EXIT
    trap 'exit 1' HUP INT TERM

    # GitHub's latest-release endpoint excludes drafts and prereleases.
    curl --fail --location --show-error --retry 3 \
        --header 'Accept: application/vnd.github+json' \
        --output "$temporary_dir/release.json" \
        https://api.github.com/repos/yorukot/superfile/releases/latest
    release_tag=$(jq -er '.tag_name | select(type == "string" and test("^v[0-9]+[.][0-9]+[.][0-9]+$"))' \
        "$temporary_dir/release.json")

    # Avoid downloading and replacing an existing installation when it already
    # matches the latest official release. Check our install location first in
    # case it is not on PATH yet.
    installed_spf=$bin_dir/spf
    if [ ! -x "$installed_spf" ]; then
        installed_spf=$(command -v spf || :)
    fi
    if [ -n "$installed_spf" ] && [ -x "$installed_spf" ]; then
        installed_version=$(
            "$installed_spf" --version 2>&1 \
                | sed -n 's/.*version v\([0-9][0-9.]*\).*/\1/p' \
                | head -n 1
        )
        if [ "$installed_version" = "${release_tag#v}" ]; then
            printf 'Superfile %s is already installed; skipping download.\n' "$release_tag"
            exit 0
        fi
    fi

    archive=superfile-linux-$release_tag-$arch
    download_url=$(jq -er --arg name "$archive.tar.gz" \
        '.assets[] | select(.name == $name) | .browser_download_url' \
        "$temporary_dir/release.json") || {
        ui_error "Superfile $release_tag has no Linux archive for $arch."
        exit 1
    }
    checksum=$(jq -er --arg name "$archive.tar.gz" \
        '.assets[] | select(.name == $name) | .digest | strings
         | select(test("^sha256:[0-9a-fA-F]{64}$")) | sub("^sha256:"; "")' \
        "$temporary_dir/release.json") || {
        ui_error "Superfile $release_tag has no valid SHA-256 digest for $arch."
        exit 1
    }

    printf 'Installing Superfile %s for %s (latest official release).\n' "$release_tag" "$arch"
    curl --fail --location --show-error --retry 3 \
        --output "$temporary_dir/$archive.tar.gz" "$download_url"
    if ! printf '%s  %s\n' "$checksum" "$temporary_dir/$archive.tar.gz" | sha256sum --check --status; then
        ui_error "Superfile $release_tag failed SHA-256 verification."
        exit 1
    fi
    tar -xzf "$temporary_dir/$archive.tar.gz" -C "$temporary_dir" "./dist/$archive/spf"

    # Stage on the same filesystem, then replace the binary atomically.
    staged_binary=$(mktemp "$bin_dir/.spf.XXXXXX")
    install -m 0755 "$temporary_dir/dist/$archive/spf" "$staged_binary"
    mv -f "$staged_binary" "$bin_dir/spf"
    staged_binary=
)

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
        gsettings set org.gnome.desktop.interface accent-color orange || :
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
    # Retire only symlinks created by older versions of this installer.
    # Preserve independently managed configurations and commands.
    for legacy_pair in \
        "$HOME/.local/bin/dotfiles-hypridle|$repo_dir/scripts/hypr/start-idle" \
        "$config_dir/hypr/hypridle.conf|$repo_dir/configs/hypr/hypridle.conf" \
        "$HOME/.local/bin/dotfiles-nnn-open|$repo_dir/scripts/nnn/open" \
        "$config_dir/nnn/env|$repo_dir/configs/nnn/env" \
        "$config_dir/nnn/plugins|$repo_dir/configs/nnn/plugins"; do
        legacy_target=${legacy_pair%%|*}
        legacy_source=${legacy_pair#*|}
        if [ -L "$legacy_target" ] && [ "$(readlink "$legacy_target")" = "$legacy_source" ]; then
            rm "$legacy_target"
        fi
    done

    link_config "$repo_dir/configs/hypr/hyprland.lua" "$config_dir/hypr/hyprland.lua"
    link_config "$repo_dir/configs/hypr/hyprlock.conf" "$config_dir/hypr/hyprlock.conf"
    link_config "$repo_dir/assets/wallpapers/default.jpg" "$HOME/.local/share/backgrounds/dotfiles-wallpaper.jpg"
    link_config "$repo_dir/scripts/hypr/set-wallpaper" "$HOME/.local/bin/dotfiles-set-wallpaper"
    link_config "$repo_dir/scripts/hypr/screenshot" "$HOME/.local/bin/dotfiles-screenshot"
    link_config "$repo_dir/scripts/hypr/start-mako" "$HOME/.local/bin/dotfiles-mako"
    link_config "$repo_dir/scripts/tui/network-tui" "$HOME/.local/bin/dotfiles-network-tui"
    link_config "$repo_dir/scripts/tui/bluetooth-tui" "$HOME/.local/bin/dotfiles-bluetooth-tui"
    link_config "$repo_dir/scripts/tui/usb-tui" "$HOME/.local/bin/dotfiles-usb-tui"
    link_config "$repo_dir/scripts/tui/partition-manager-tui" "$HOME/.local/bin/dotfiles-partition-manager-tui"
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
    link_config "$repo_dir/scripts/tui/agent-tui" "$HOME/.local/bin/dotfiles-agent-tui"
    link_config "$repo_dir/scripts/tui/tasks-tui" "$HOME/.local/bin/tasks"
    link_config "$repo_dir/scripts/tui/tasks-tui" "$HOME/.local/bin/dotfiles-tasks-tui"
    for desktop_file in "$repo_dir"/configs/applications/*.desktop; do
        link_config "$desktop_file" "$HOME/.local/share/applications/$(basename "$desktop_file")"
    done
    link_config "$repo_dir/scripts/superfile/open" "$HOME/.local/bin/dotfiles-superfile-open"
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
    link_config "$repo_dir/configs/foot/foot.ini" "$config_dir/foot/foot.ini"
    link_config "$repo_dir/configs/micro/settings.json" "$config_dir/micro/settings.json"
    link_config "$repo_dir/configs/micro/colorschemes/dotfiles.micro" "$config_dir/micro/colorschemes/dotfiles.micro"
    link_config "$repo_dir/configs/micro/syntax/css.yaml" "$config_dir/micro/syntax/css.yaml"
    link_config "$repo_dir/configs/superfile/env" "$config_dir/superfile/env"
    link_config "$repo_dir/configs/superfile/config.toml" "$config_dir/superfile/config.toml"
    link_config "$repo_dir/configs/superfile/theme/dotfiles.toml" "$config_dir/superfile/theme/dotfiles.toml"
    link_config "$repo_dir/configs/bash/bashrc" "$HOME/.bashrc"

}

configure_agent_timer() {
    link_config "$repo_dir/configs/systemd/user/dotfiles-agent.service" "$config_dir/systemd/user/dotfiles-agent.service"
    link_config "$repo_dir/configs/systemd/user/dotfiles-agent.timer" "$config_dir/systemd/user/dotfiles-agent.timer"
    link_config "$repo_dir/configs/systemd/user/dotfiles-agent.timer" "$config_dir/systemd/user/timers.target.wants/dotfiles-agent.timer"
    if systemctl --user daemon-reload && systemctl --user start dotfiles-agent.timer; then
        ui_success "Seth background timer is running."
    else
        printf '%s\n' 'Seth background timer is enabled for future user sessions; it could not start now.' >&2
    fi
}

main() {
    ui_init
    ui_banner
    ui_step "Checking system requirements"
    check_platform

    ui_step "Configuring Debian package sources"
    configure_debian_sources
    configure_backports

    ui_step "Installing desktop packages"
    install_packages

    ui_step "Installing Superfile"
    install_superfile

    ui_step "Building Tasks"
    "$repo_dir/scripts/tasks/build.sh"

    ui_step "Preparing native Seth"
    "$repo_dir/scripts/agent/setup.sh"

    ui_step "Installing fonts and configuring Flatpak"
    install_font
    configure_flatpak

    ui_step "Setting up the LightDM login screen"
    configure_lightdm

    ui_step "Configuring networking, power, and printing"
    configure_networking
    configure_hardware_services
    configure_pam

    ui_step "Setting up the desktop session and user configuration"
    configure_session
    link_configs
    xdg-mime default dotfiles-file-manager.desktop inode/directory
    sed -n 's/^MimeType=//p' "$repo_dir/configs/applications/dotfiles-text-editor.desktop" |
        tr ';' '\n' |
        while IFS= read -r mime_type; do
            [ -n "$mime_type" ] || continue
            xdg-mime default dotfiles-text-editor.desktop "$mime_type"
        done
    configure_theme

    configure_agent_timer

    if [ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
        systemctl --user daemon-reload || :
        systemctl --user enable --now foot-server.socket waybar.service mako.service || :
        systemctl --user try-restart hyprpolkitagent.service xdg-desktop-portal-hyprland.service || :
    fi

    ui_success "Installation complete. Log out and back in to start the TruDE desktop."
}

main "$@"

#!/bin/sh
set -eu

repo_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd -P)
activate=false
case ${1:-} in
    '') ;;
    --activate) activate=true ;;
    *) printf 'Usage: sh %s [--activate]\n' "$0" >&2; exit 1 ;;
esac
if [ "$(id -u)" -eq 0 ]; then
    printf 'Run this script as your normal desktop user.\n' >&2
    exit 1
fi
if [ ! -x /usr/bin/uwsm ] || [ ! -f /usr/share/wayland-sessions/hyprland.desktop ]; then
    printf 'Install the TruDE desktop with install.sh before configuring its login screen.\n' >&2
    exit 1
fi

# Temporary migration: check before installing anything that could stop LightDM.
check_lightdm_migration() {
    if systemctl is-active --quiet lightdm.service; then
        case $(tty 2>/dev/null || :) in
            /dev/tty[0-9]*) ;;
            *) printf 'Switch to a text console with Ctrl+Alt+F5, log in, and run this script there to replace LightDM.\n' >&2; exit 1 ;;
        esac
    fi
}

# Temporary migration: retire LightDM and only the settings owned by TruDE.
remove_lightdm() {
    old_packages=
    for package in lightdm slick-greeter lightdm-settings; do
        case $(dpkg-query -W -f='${Status}' "$package" 2>/dev/null || :) in
            'install ok installed'|'deinstall ok config-files') old_packages="$old_packages $package" ;;
        esac
    done
    if [ -n "$old_packages" ]; then
        if systemctl cat lightdm.service >/dev/null 2>&1; then
            sudo systemctl disable lightdm.service
        fi
        # Keep unrelated automatically installed packages in place.
        # shellcheck disable=SC2086
        sudo apt-get purge -y --no-auto-remove -- $old_packages </dev/tty
    fi
    sudo rm -f -- /etc/lightdm/lightdm.conf.d/50-TruDE.conf \
        /var/lib/lightdm/.config/gtk-3.0/gtk.css
    if [ "$(cat "$HOME/.dmrc" 2>/dev/null || :)" = "$(printf '[Desktop]\nSession=hyprland-uwsm')" ]; then
        rm -f -- "$HOME/.dmrc"
    fi
}

# Temporary migration: use the direct UWSM command and Debian's greetd service.
remove_greetd_launch_overrides() {
    sudo rm -f -- /usr/local/bin/TruDE-session \
        /usr/local/share/TruDE/sessions/TruDE.desktop \
        /etc/systemd/system/greetd.service.d/console.conf
    for old_dir in /usr/local/share/TruDE/sessions /usr/local/share/TruDE/xsessions \
        /etc/systemd/system/greetd.service.d; do
        if [ -d "$old_dir" ]; then
            sudo rmdir --ignore-fail-on-non-empty -- "$old_dir"
        fi
    done
}

check_lightdm_migration
if [ "$activate" = true ]; then
    case $(tty 2>/dev/null || :) in
        /dev/tty[0-9]*) ;;
        *) printf 'Use --activate from a text console (Ctrl+Alt+F5).\n' >&2; exit 1 ;;
    esac
fi

missing_packages=
for package in greetd tuigreet libpam-systemd libpam-gnome-keyring gnome-keyring; do
    if [ "$(dpkg-query -W -f='${Status}' "$package" 2>/dev/null || :)" != 'install ok installed' ]; then
        missing_packages="$missing_packages $package"
    fi
done
if [ -n "$missing_packages" ]; then
    # Installing a login manager must not take over the console running setup.
    sudo systemctl mask --runtime greetd.service
    # shellcheck disable=SC2086
    sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends -- $missing_packages </dev/tty
fi

if ! getent passwd trude-greeter >/dev/null; then
    sudo adduser --system --group --home /var/lib/TruDE-greeter \
        --no-create-home --shell /usr/sbin/nologin trude-greeter
fi
sudo install -d -m 0750 -o trude-greeter -g trude-greeter \
    /var/lib/TruDE-greeter /var/cache/tuigreet
sudo install -D -m 0755 "$repo_dir/scripts/greetd/greeter" /usr/local/libexec/TruDE-greet
sudo install -D -m 0644 "$repo_dir/configs/greetd/config.toml" /etc/greetd/config.toml
sudo install -D -m 0644 "$repo_dir/configs/greetd/power.rules" /etc/polkit-1/rules.d/50-TruDE-greeter-power.rules

# Keep the distribution's authentication policy and add keyring integration once.
pam_file=/etc/pam.d/greetd
if [ ! -r "$pam_file" ]; then
    printf 'The greetd package PAM policy is missing. Reinstall greetd before replacing the login manager.\n' >&2
    exit 1
fi
if ! grep -Eq '^[[:space:]]*-?auth[[:space:]].*pam_gnome_keyring[.]so([[:space:]]|$)' "$pam_file"; then
    printf '%s\n' 'auth optional pam_gnome_keyring.so' | sudo tee -a "$pam_file" >/dev/null
fi
if ! grep -Eq '^[[:space:]]*-?session[[:space:]].*pam_gnome_keyring[.]so.*[[:space:]]auto_start([[:space:]]|$)' "$pam_file"; then
    printf '%s\n' \
        'session [success=1 default=ignore] pam_succeed_if.so user = trude-greeter' \
        'session optional pam_gnome_keyring.so auto_start' | sudo tee -a "$pam_file" >/dev/null
fi

remove_lightdm
remove_greetd_launch_overrides
sudo systemctl daemon-reload
sudo systemctl unmask --runtime greetd.service
sudo systemctl enable --force greetd.service
greetd_binary=$(sudo sh -c 'command -v greetd')
printf '%s\n' "$greetd_binary" | sudo tee /etc/X11/default-display-manager >/dev/null
sudo systemctl set-default graphical.target

if [ "$activate" = true ]; then
    sudo systemctl restart greetd.service
else
    printf 'greetd + tuigreet configured. Reboot, or run this script with --activate from a text console.\n'
fi

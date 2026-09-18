#!/bin/bash
set -euo pipefail

backports=/etc/apt/sources.list.d/dotfiles-backports.sources
backports_suite=trixie-backports

if [[ ${EUID} -eq 0 ]]; then
    echo "Run this script as your normal desktop user, not root." >&2
    exit 1
fi

if ! command -v sudo >/dev/null 2>&1; then
    echo "sudo is required." >&2
    exit 1
fi

. /etc/os-release
if [[ ${ID:-} != debian || ${VERSION_CODENAME:-} != trixie ]]; then
    echo "This script supports Debian 13 (trixie)." >&2
    exit 1
fi

configure_backports() {
    sudo tee "$backports" >/dev/null <<EOF2
Types: deb
URIs: https://deb.debian.org/debian
Suites: $backports_suite
Components: main contrib non-free non-free-firmware
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg
EOF2
}

detect_gpu_vendors() {
    local vendor_file vendor_id

    for vendor_file in /sys/bus/pci/devices/*/vendor; do
        [[ -r $vendor_file ]] || continue
        vendor_id=$(<"$vendor_file")
        case $vendor_id in
            0x1002) printf '%s\n' amd ;;
            0x10de) printf '%s\n' nvidia ;;
            0x8086) printf '%s\n' intel ;;
        esac
    done | sort -u
}

package_is_available() {
    apt-cache policy "$1" | awk -v suite="$backports_suite" \
        '$0 ~ suite { found=1 } END { exit !found }'
}

append_available() {
    local -n target=$1
    shift

    local package
    for package in "$@"; do
        if package_is_available "$package"; then
            target+=("$package")
        fi
    done
}

configure_i386() {
    if ! dpkg --print-foreign-architectures | grep -qx i386; then
        sudo dpkg --add-architecture i386
    fi
}

install_stack() {
    local -a gpu_vendors
    mapfile -t gpu_vendors < <(detect_gpu_vendors)

    local -a packages=()
    append_available packages \
        linux-image-amd64 \
        linux-headers-amd64 \
        mesa-vulkan-drivers \
        libgl1-mesa-dri \
        libglx-mesa0 \
        libgbm1 \
        mesa-va-drivers \
        mesa-vdpau-drivers \
        libvulkan1 \
        vulkan-tools \
        vulkan-validationlayers

    local vendor
    for vendor in "${gpu_vendors[@]}"; do
        case $vendor in
            amd)
                append_available packages firmware-amd-graphics
                ;;
            intel)
                append_available packages firmware-intel-graphics
                ;;
            nvidia)
                append_available packages firmware-nvidia-graphics nvidia-driver nvidia-vulkan-icd
                ;;
        esac
    done

    if ((${#packages[@]} == 0)); then
        echo "No supported backports packages were found." >&2
        exit 1
    fi

    sudo apt-get install -y -t "$backports_suite" "${packages[@]}"

    if dpkg --print-foreign-architectures | grep -qx i386; then
        local -a multilib_packages=()
        append_available multilib_packages \
            mesa-vulkan-drivers:i386 \
            libgl1-mesa-dri:i386 \
            libglx-mesa0:i386 \
            libgbm1:i386 \
            libvulkan1:i386

        if ((${#multilib_packages[@]} > 0)); then
            sudo apt-get install -y -t "$backports_suite" "${multilib_packages[@]}"
        fi
    fi
}

main() {
    configure_backports
    configure_i386
    sudo apt-get update
    install_stack

    echo
    echo "Backported kernel and gaming graphics stack installed."
    echo "Reboot to start using a newly installed kernel or graphics driver."
}

main "$@"

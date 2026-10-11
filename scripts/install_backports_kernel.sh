#!/bin/sh
set -eu

. /etc/os-release
[ "${ID:-}" = debian ] && [ -n "${VERSION_CODENAME:-}" ] || {
    echo "This script requires Debian with a release codename." >&2
    exit 1
}
architecture=$(dpkg --print-architecture)
case $architecture in
    amd64|arm64) ;;
    *)
        printf 'No backports kernel is configured for %s.\n' "$architecture" >&2
        exit 1
        ;;
esac
backports_suite=$VERSION_CODENAME-backports

sudo apt-get update
# Select backports for the whole transaction so new kernel dependencies can
# upgrade from stable when required, not just the kernel metapackages.
sudo apt-get install -y -t "$backports_suite" "linux-image-$architecture" "linux-headers-$architecture"

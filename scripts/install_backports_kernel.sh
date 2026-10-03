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
sudo apt-get install -y "linux-image-$architecture/$backports_suite" "linux-headers-$architecture/$backports_suite"

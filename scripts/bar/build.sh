#!/bin/sh
set -eu
base=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$base/bin"
scanner=${WAYLAND_SCANNER:-wayland-scanner}
"$scanner" client-header "$base/layer-shell.xml" "$base/bin/layer-shell.h"
"$scanner" private-code "$base/layer-shell.xml" "$base/bin/layer-shell.c"
"$scanner" client-header /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml "$base/bin/xdg-shell.h"
"$scanner" private-code /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml "$base/bin/xdg-shell.c"
output=$(mktemp "$base/bin/.bar.XXXXXX")
trap 'rm -f "$output"' 0 HUP INT TERM
packages="wayland-client wayland-cursor pangocairo libsystemd libpipewire-0.3 json-c libpulse"
cflags=$(pkg-config --cflags $packages)
libs=$(pkg-config --libs $packages)
${CC:-cc} -std=gnu11 -O2 -Wall -Wextra -Wno-unused-function -Wno-unused-variable -fstack-protector-strong \
    $cflags \
    -I"$base/bin" "$base/bar.c" "$base/bin/layer-shell.c" "$base/bin/xdg-shell.c" \
    -o "$output" $libs -lm
chmod 755 "$output"
mv "$output" "$base/bin/bar"

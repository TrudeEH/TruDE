#!/bin/sh
set -eu
base=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$base/bin"
output=$(mktemp "$base/bin/.status.XXXXXX")
trap 'rm -f "$output"' EXIT
# Deliberate word splitting: pkg-config returns compiler/linker argument lists.
${CC:-cc} -std=gnu11 -O2 -Wall -Wextra -fstack-protector-strong \
    $(pkg-config --cflags libsystemd libpipewire-0.3 json-c) \
    "$base/status.c" -o "$output" \
    $(pkg-config --libs libsystemd libpipewire-0.3 json-c)
chmod 755 "$output"
mv "$output" "$base/bin/status"

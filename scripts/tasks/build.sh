#!/bin/sh
set -eu
base=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$base/bin"
output=$(mktemp "$base/bin/.tasks.XXXXXX")
trap 'rm -f "$output"' EXIT
${CC:-cc} -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Wpedantic \
    -fstack-protector-strong "$base/tasks.c" -o "$output"
chmod 755 "$output"
mv "$output" "$base/bin/tasks"

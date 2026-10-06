#!/bin/sh
set -eu
base=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$base/bin"
output=$(mktemp "$base/bin/.tasks.XXXXXX")
trap 'rm -f "$output"' EXIT
${CC:-cc} -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Wpedantic \
    -fstack-protector-strong -I"$base" ${TASKS_CFLAGS:-} \
    "$base/tasks.c" "$base/backend.c" "$base/json.c" -o "$output" ${TASKS_LIBS:--lcurl} -lm
chmod 755 "$output"
mv "$output" "$base/bin/tasks"

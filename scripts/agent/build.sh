#!/bin/sh
set -eu
agent_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
output=${1:-"$agent_dir/bin/seth"}
mkdir -p "$(dirname -- "$output")"
compiler=${CC:-cc}
if ! command -v "$compiler" >/dev/null 2>&1; then
    printf '%s\n' 'A C compiler is required to build Seth. Install build-essential.' >&2
    exit 1
fi
temporary=$(mktemp "$(dirname -- "$output")/.seth.XXXXXX")
trap 'rm -f -- "$temporary"' EXIT HUP INT TERM
"$compiler" -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow \
    -fstack-protector-strong -D_FORTIFY_SOURCE=2 -pthread \
    -I"$agent_dir/src" "$agent_dir"/src/*.c "$agent_dir"/src/servers/*.c \
    -o "$temporary" -Wl,-z,relro,-z,now ${LDFLAGS:-}
chmod 755 "$temporary"
mv -- "$temporary" "$output"
printf 'Built %s\n' "$output"

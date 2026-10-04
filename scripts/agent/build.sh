#!/bin/sh
set -eu
agent_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
machine=$(uname -m)
case "$machine" in
    x86_64) arch=amd64 ;;
    aarch64) arch=arm64 ;;
    *) arch=$machine ;;
esac
output=${1:-"$agent_dir/bin/seth-$arch"}
mkdir -p "$(dirname -- "$output")"
compiler=${CC:-cc}
if ! command -v "$compiler" >/dev/null 2>&1; then
    printf '%s\n' 'A C compiler is needed only to rebuild Seth; the supplied binary needs no compiler.' >&2
    exit 1
fi
"$compiler" -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow \
    -fstack-protector-strong -D_FORTIFY_SOURCE=2 -pthread -static-pie -s \
    "$agent_dir"/src/*.c -o "$output" -Wl,-z,relro,-z,now ${LDFLAGS:-}
if [ "$output" = "$agent_dir/bin/seth-$arch" ]; then
    (cd "$agent_dir/bin" && sha256sum seth-* > SHA256SUMS)
fi
printf 'Built %s\n' "$output"

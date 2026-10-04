#!/bin/sh
set -eu
agent_source=$(CDPATH='' cd -- "$(dirname -- "$(readlink -f -- "$0")")" && pwd)
case $(uname -m) in
    x86_64) agent_arch=amd64 ;;
    aarch64) agent_arch=arm64 ;;
    *) agent_arch=$(uname -m) ;;
esac
if ! command -v curl >/dev/null 2>&1; then
    printf 'Seth uses curl, which is already included in the dotfiles Debian package list.\n' >&2
    exit 1
fi
if [ ! -x "$agent_source/bin/seth-$agent_arch" ]; then
    "$agent_source/build.sh"
fi
if [ -f "$agent_source/bin/SHA256SUMS" ]; then
    if ! (cd "$agent_source/bin" && sha256sum --check --status SHA256SUMS); then
        printf 'The Seth binary checksum does not match. Rebuild with scripts/agent/build.sh.\n' >&2
        exit 1
    fi
fi
printf 'Native Seth is ready. Start dotfiles-agent-tui. No runtime installation is needed.\n'

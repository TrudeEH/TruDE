#!/bin/sh
set -eu
agent_source=$(CDPATH='' cd -- "$(dirname -- "$(readlink -f -- "$0")")" && pwd)
agent_runtime=${XDG_DATA_HOME:-"$HOME/.local/share"}/dotfiles-agent/runtime
node -e 'if (Number(process.versions.node.split(".")[0]) < 20) { console.error("Node.js 20 or newer is required."); process.exit(1); }'
mkdir -p "$agent_runtime"
chmod 700 "$agent_runtime"
cp "$agent_source/package.json" "$agent_source/package-lock.json" "$agent_runtime/"
npm ci --prefix "$agent_runtime" --ignore-scripts --no-audit --no-fund
printf 'Seth dependencies installed. Start dotfiles-agent-tui.\n'

#!/bin/sh
# Compatibility entry point; installation always builds from source.
set -eu
agent_dir=$(CDPATH='' cd -- "$(dirname -- "$(readlink -f -- "$0")")" && pwd)
exec "$agent_dir/build.sh" "$@"

#!/bin/sh
set -eu
agent_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_dir=$(mktemp -d /tmp/seth-tests.XXXXXX)
trap 'rm -rf -- "$test_dir"' EXIT HUP INT TERM
"$agent_dir/build.sh" "$test_dir/seth"
export XDG_CONFIG_HOME="$test_dir/config"
export XDG_DATA_HOME="$test_dir/data"
export XDG_STATE_HOME="$test_dir/state"
"$test_dir/seth" --self-test
if command -v python3 >/dev/null 2>&1; then
    python3 "$agent_dir/test/9router.py" "$test_dir/seth"
    python3 "$agent_dir/test/native.py" "$test_dir/seth"
fi

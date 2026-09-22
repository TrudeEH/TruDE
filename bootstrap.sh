#!/bin/sh
set -eu

data_dir=${XDG_DATA_HOME:-"$HOME/.local/share"}
install_dir=$data_dir/trude
archive=$(mktemp)
trap 'rm -f "$archive"' 0

mkdir -p "$install_dir"
curl --fail --location --silent --show-error \
    https://codeload.github.com/TrudeEH/TruDE/tar.gz/refs/heads/master \
    --output "$archive"
tar -xzf "$archive" --strip-components=1 -C "$install_dir"
exec "$install_dir/install.sh" "$@"

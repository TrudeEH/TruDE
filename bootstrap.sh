#!/bin/sh
set -eu

repo_url=https://github.com/TrudeEH/TruDE.git
repo_dir=$HOME/dotfiles

sudo apt-get update
sudo apt-get install -y git </dev/tty

if [ -d "$repo_dir/.git" ]; then
    origin=$(git -C "$repo_dir" remote get-url origin)
    case $origin in
        https://github.com/TrudeEH/TruDE|https://github.com/TrudeEH/TruDE.git) ;;
        *)
            echo "$repo_dir is already a Git repository with a different origin: $origin" >&2
            exit 1
            ;;
    esac
    git -C "$repo_dir" pull --ff-only
elif [ -e "$repo_dir" ]; then
    echo "$repo_dir already exists and is not a Git checkout; move it before installing TruDE." >&2
    exit 1
else
    git clone "$repo_url" "$repo_dir"
fi

exec "$repo_dir/install.sh" "$@"

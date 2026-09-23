#!/bin/sh
set -eu

ui_init() {
    ui_reset=
    ui_accent=
    ui_success_color=
    ui_error_color=
    ui_muted=
    if [ -t 1 ] && [ -z "${NO_COLOR+x}" ] && [ "${TERM:-dumb}" != dumb ]; then
        ui_reset=$(printf '\033[0m')
        ui_accent=$(printf '\033[1;38;5;214m')
        ui_success_color=$(printf '\033[1;38;5;77m')
        ui_error_color=$(printf '\033[1;38;5;203m')
        ui_muted=$(printf '\033[38;5;245m')
    fi
}

ui_banner() {
    printf '\n%s  TruDE%s\n' "$ui_accent" "$ui_reset"
    printf '%s  Debian desktop setup%s\n' "$ui_muted" "$ui_reset"
}

ui_step() {
    printf '\n%s==>%s %s\n' "$ui_accent" "$ui_reset" "$1"
}

ui_error() {
    printf '%sError:%s %s\n' "$ui_error_color" "$ui_reset" "$1" >&2
}

repo_url=https://github.com/TrudeEH/TruDE.git
repo_dir=$HOME/dotfiles

ui_init
ui_banner
ui_step "Preparing the package manager"
sudo apt-get update
sudo apt-get install -y git </dev/tty

if [ -d "$repo_dir/.git" ]; then
    ui_step "Updating TruDE"
    origin=$(git -C "$repo_dir" remote get-url origin)
    case $origin in
        https://github.com/TrudeEH/TruDE|https://github.com/TrudeEH/TruDE.git) ;;
        *)
            ui_error "$repo_dir is already a Git repository with a different origin: $origin"
            exit 1
            ;;
    esac
    git -C "$repo_dir" pull --ff-only
elif [ -e "$repo_dir" ]; then
    ui_error "$repo_dir already exists and is not a Git checkout; move it before installing TruDE."
    exit 1
else
    ui_step "Downloading TruDE"
    git clone "$repo_url" "$repo_dir"
fi

ui_step "Starting desktop setup"
exec "$repo_dir/install.sh" "$@"

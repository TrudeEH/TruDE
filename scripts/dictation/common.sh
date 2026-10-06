#!/bin/sh
# Shared paths and validated model names. No user configuration is executed.
export PYTHONDONTWRITEBYTECODE=1
config_home=${XDG_CONFIG_HOME:-"$HOME/.config"}
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
preferences_file=$config_home/dotfiles/settings.json
model_dir=$data_home/dotfiles/dictation/models
engine=$data_home/dotfiles/dictation/bin/whisper-cli
runtime=${XDG_RUNTIME_DIR:?A user runtime directory is required}/dotfiles-dictation
mkdir -p "$runtime"
chmod 700 "$runtime"
dictation_root=$data_home/dotfiles/dictation
sherpa_python=$dictation_root/backends/sherpa/bin/python
catalog=$(dirname -- "$(readlink -f -- "$0")")/catalog.json
# TUI sources this file from a sibling directory.
[ -f "$catalog" ] || catalog=$(dirname -- "$(readlink -f -- "$0")")/../dictation/catalog.json
model_valid() {
    case $1 in tiny|tiny.en|base|base.en|small|small.en|medium|medium.en|large-v3-turbo|moonshine-tiny|moonshine-base|parakeet-v3|qwen3-0.6b) return 0 ;; *) return 1 ;; esac
}
model=$(jq -r '.dictation.model // "base"' "$preferences_file" 2>/dev/null || printf base)
model_valid "$model" || model=base
refresh_launcher() {
    # Refresh the original center and both dividers with the same realtime signal.
    pkill -RTMIN+9 -u "$(id -u)" -x waybar 2>/dev/null || :
}
state() {
    printf '%s\n' "$1" > "$runtime/state.new"
    mv "$runtime/state.new" "$runtime/state"
    refresh_launcher
}

backend_for() {
    case $1 in moonshine-*|parakeet-v3|qwen3-0.6b) printf sherpa ;; *) printf whisper ;; esac
}
model_ready() {
    if [ "$(backend_for "$1")" = sherpa ]; then
        [ -x "$sherpa_python" ] && [ -f "$model_dir/$1/.ready" ]
    else
        [ -x "$engine" ] && [ -s "$model_dir/ggml-$1.bin" ]
    fi
}

model_status() {
    if model_ready "$1"; then
        printf 'Downloaded'
    elif [ -d "$model_dir/$1" ] || [ -f "$model_dir/ggml-$1.bin" ]; then
        printf 'Incomplete / runtime missing'
    else
        printf 'Not downloaded'
    fi
}
# Install only missing Debian packages, after explicit confirmation. Never remove
# system packages on model deletion: other applications may use them.
ensure_packages() {
    missing=
    for package do
        if [ "$(dpkg-query -W -f='${Status}' "$package" 2>/dev/null || :)" != 'install ok installed' ]; then
            missing="$missing $package"
        fi
    done
    [ -n "$missing" ] || return 0
    printf '\nThis runtime requires these missing Debian packages:%s\nInstall them with sudo? [y/N] ' "$missing"
    read -r answer || return 1
    case $answer in y|Y|yes|YES) ;; *) return 1 ;; esac
    sudo apt-get update
    # Package names originate only from fixed caller arguments.
    # shellcheck disable=SC2086
    sudo apt-get install --no-install-recommends -- $missing
}

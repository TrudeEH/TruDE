# shellcheck shell=sh
# Keep fallback caches private to the desktop user when no session runtime
# directory is available. XDG_RUNTIME_DIR is already private on Debian.
dotfiles_runtime_dir() (
    if [ -n "${XDG_RUNTIME_DIR:-}" ]; then
        printf '%s\n' "$XDG_RUNTIME_DIR"
        exit 0
    fi
    umask 077
    runtime_uid=$(id -u)
    runtime_base=${TMPDIR:-/tmp}/dotfiles-$runtime_uid
    mkdir -p "$runtime_base" || exit 1
    [ ! -L "$runtime_base" ] && [ "$(stat -c %u "$runtime_base")" = "$runtime_uid" ] || exit 1
    chmod 700 "$runtime_base" || exit 1
    printf '%s\n' "$runtime_base"
)

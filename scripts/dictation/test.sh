#!/bin/sh
# Isolated integration tests: no microphone, model download, clipboard or systemd.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
temporary=$(mktemp -d "$repo/scripts/dictation/test.XXXXXX")
trap 'rm -rf "$temporary"' EXIT
export HOME=$temporary/home XDG_RUNTIME_DIR=$temporary/runtime
export XDG_CONFIG_HOME=$temporary/config XDG_DATA_HOME=$temporary/data
export PATH=$temporary/bin:$PATH
export TEST_ROOT=$temporary WAYLAND_DISPLAY=test-wayland
mkdir -p "$HOME" "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME/dotfiles" "$temporary/bin" "$XDG_DATA_HOME/dotfiles/dictation/bin" "$XDG_DATA_HOME/dotfiles/dictation/models"
cat > "$temporary/bin/systemctl" <<'MOCK'
#!/bin/sh
case $* in *is-active*) exit 1 ;; *) printf '%s\n' "$*" >> "$TEST_ROOT/systemctl" ;; esac
MOCK
cat > "$temporary/bin/pw-record" <<'MOCK'
#!/bin/sh
for last do :; done
printf audio > "$last"
: > "$XDG_RUNTIME_DIR/dotfiles-dictation/stop"
sleep 20
MOCK
cat > "$temporary/bin/hyprctl" <<'MOCK'
#!/bin/sh
case $1 in activewindow) printf '{"class":"%s","address":"0x123"}\n' "${TEST_CLASS:-foot}" ;; dispatch)
    case $2 in hl.dsp.send_shortcut*) ;; *) printf 'error: legacy dispatcher syntax\n'; exit 1 ;; esac
    printf '%s\n' "$*" > "$TEST_ROOT/paste"
    if [ "${TEST_DISPATCH_ERROR:-false}" = true ]; then
        printf 'error: rejected shortcut\n'
        exit 0
    fi ;; esac
MOCK
cat > "$temporary/bin/pkill" <<'MOCK'
#!/bin/sh
printf '%s\n' "$*" >> "$TEST_ROOT/signals"
MOCK
cat > "$temporary/bin/systemd-run" <<'MOCK'
#!/bin/sh
printf '%s\n' "$*" > "$TEST_ROOT/clipboard-unit"
while [ "$1" != -- ]; do shift; done
shift
exec "$@"
MOCK
cat > "$temporary/bin/wl-paste" <<'MOCK'
#!/bin/sh
cat "$TEST_ROOT/clipboard"
MOCK
cat > "$temporary/bin/wl-copy" <<'MOCK'
#!/bin/sh
for text do :; done
printf '%s\n' "$text" > "$TEST_ROOT/clipboard"
MOCK
cat > "$XDG_DATA_HOME/dotfiles/dictation/bin/whisper-cli" <<'MOCK'
#!/bin/sh
while [ "$#" -gt 0 ]; do
    if [ "$1" = -of ]; then shift; output=$1; fi
    shift
done
printf '  Hello world.\n Another sentence.\n' > "$output.txt"
MOCK
chmod +x "$temporary/bin/"* "$XDG_DATA_HOME/dotfiles/dictation/bin/whisper-cli"
printf model > "$XDG_DATA_HOME/dotfiles/dictation/models/ggml-base.bin"
"$repo/scripts/dictation/control"
[ "$(cat "$XDG_RUNTIME_DIR/dotfiles-dictation/state")" = dictation-error ]
printf '{"dictation":{"enabled":true,"model":"base"}}\n' > "$XDG_CONFIG_HOME/dotfiles/settings.json"
"$repo/scripts/dictation/control"
grep -q 'start dotfiles-dictation.service' "$temporary/systemctl"
"$repo/scripts/dictation/worker"
[ "$(cat "$temporary/clipboard")" = 'Hello world. Another sentence.' ]
grep -Fq 'hl.dsp.send_shortcut({mods="CTRL_SHIFT",key="v",window="address:0x123"})' "$temporary/paste"
grep -q -- '--foreground --type text/plain' "$temporary/clipboard-unit"
grep -q -- '-RTMIN+9' "$temporary/signals"
TEST_CLASS=firefox "$repo/scripts/dictation/worker"
grep -Fq 'hl.dsp.send_shortcut({mods="CTRL",key="v",window="address:0x123"})' "$temporary/paste"
[ ! -f "$XDG_RUNTIME_DIR/dotfiles-dictation/state" ]
[ -z "$(find "$XDG_RUNTIME_DIR/dotfiles-dictation" -name 'audio.*' -print)" ]
# A dispatcher error can be printed with exit status zero. Do not report success.
if TEST_DISPATCH_ERROR=true "$repo/scripts/dictation/worker" 2>/dev/null; then exit 1; fi
grep -q 'Hyprland rejected' "$XDG_RUNTIME_DIR/dotfiles-dictation/error"
# Errors and stale state must leave the launcher untouched.
cat > "$temporary/bin/pw-cli" <<'MOCK'
#!/bin/sh
exit 0
MOCK
chmod +x "$temporary/bin/pw-cli"
for inactive in dictation-error recording transcribing; do
    printf '%s\n' "$inactive" > "$XDG_RUNTIME_DIR/dotfiles-dictation/state"
    rm -f "$XDG_RUNTIME_DIR/dotfiles-waybar-launcher-status"
    "$repo/scripts/waybar/launcher-status" center | jq -e '.class == "launcher"' >/dev/null
done
exec 7>"$XDG_RUNTIME_DIR/dotfiles-dictation/worker.lock"
flock 7
rm -f "$XDG_RUNTIME_DIR/dotfiles-waybar-launcher-status"
printf recording > "$XDG_RUNTIME_DIR/dotfiles-dictation/state"
"$repo/scripts/waybar/launcher-status" center | jq -e '.class == "recording" and (.tooltip | contains("Super+D"))' >/dev/null
printf transcribing > "$XDG_RUNTIME_DIR/dotfiles-dictation/state"
rm -f "$XDG_RUNTIME_DIR/dotfiles-waybar-launcher-status"
"$repo/scripts/waybar/launcher-status" center | jq -e '.class == "transcribing"' >/dev/null
for part in left right; do
    "$repo/scripts/waybar/launcher-status" "$part" | jq -e '.class == "transcribing"' >/dev/null
done
# No cached timer state: the next call must observe this change immediately.
printf recording > "$XDG_RUNTIME_DIR/dotfiles-dictation/state"
"$repo/scripts/waybar/launcher-status" center | jq -e '.class == "recording"' >/dev/null
# Removing one model preserves a runtime still needed by another.
flock -u 7
mkdir -p "$XDG_DATA_HOME/dotfiles/dictation/backends/sherpa/bin" "$XDG_DATA_HOME/dotfiles/dictation/models/moonshine-tiny" "$XDG_DATA_HOME/dotfiles/dictation/models/parakeet-v3"
printf loader > "$XDG_DATA_HOME/dotfiles/dictation/backends/sherpa/bin/python"
"$repo/scripts/dictation/remove" moonshine-tiny
[ ! -d "$XDG_DATA_HOME/dotfiles/dictation/models/moonshine-tiny" ]
[ -d "$XDG_DATA_HOME/dotfiles/dictation/backends/sherpa" ]
"$repo/scripts/dictation/remove" parakeet-v3
[ ! -d "$XDG_DATA_HOME/dotfiles/dictation/backends/sherpa" ]
"$repo/scripts/dictation/remove" base
[ ! -f "$XDG_DATA_HOME/dotfiles/dictation/bin/whisper-cli" ]
# Deletion cannot race an active recognizer or installer.
flock 7
exec 6>"$XDG_RUNTIME_DIR/dotfiles-dictation/setup.lock"
flock -s 6
if "$repo/scripts/dictation/remove" small 2>/dev/null; then exit 1; fi
flock -u 6
# Status labels never import a runtime; package installation requires consent.
cat > "$temporary/bin/dpkg-query" <<'MOCK'
#!/bin/sh
case $* in *already-present*) printf 'install ok installed' ;; *) exit 1 ;; esac
MOCK
cat > "$temporary/bin/sudo" <<'MOCK'
#!/bin/sh
printf '%s\n' "$*" >> "$TEST_ROOT/packages"
MOCK
chmod +x "$temporary/bin/dpkg-query" "$temporary/bin/sudo"
COMMON=$repo/scripts/dictation/common.sh
export COMMON
sh -c '. "$COMMON"; [ "$(model_status small)" = "Not downloaded" ]; ensure_packages already-present'
[ ! -e "$temporary/packages" ]
if printf 'n\n' | sh -c '. "$COMMON"; ensure_packages python3-venv' >/dev/null; then exit 1; fi
[ ! -e "$temporary/packages" ]
printf 'y\n' | sh -c '. "$COMMON"; ensure_packages python3-venv already-present' >/dev/null
grep -q 'install --no-install-recommends -- python3-venv$' "$temporary/packages"
if grep -q 'already-present' "$temporary/packages"; then exit 1; fi
printf 'Dictation integration tests passed.\n'

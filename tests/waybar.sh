#!/bin/sh
# Integration checks with isolated settings and mocked desktop commands.
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' 0 HUP INT TERM
export XDG_CONFIG_HOME=$test_dir/config
export XDG_RUNTIME_DIR=$test_dir/runtime
export WAYBAR_TEST_DIR=$test_dir
mkdir -p "$XDG_CONFIG_HOME/waybar" "$XDG_CONFIG_HOME/dotfiles" "$XDG_RUNTIME_DIR" "$test_dir/bin"
cp "$repo_dir/configs/waybar/config.jsonc" "$XDG_CONFIG_HOME/waybar/config.jsonc"
cp "$repo_dir/configs/waybar/style.css" "$XDG_CONFIG_HOME/waybar/style.css"
for command in waybar hyprctl systemctl whiptail; do
    ln -s "$repo_dir/tests/waybar-command" "$test_dir/bin/$command"
done
PATH=$test_dir/bin:$PATH
export PATH
preferences=$XDG_CONFIG_HOME/dotfiles/settings.json
generated=$XDG_RUNTIME_DIR/dotfiles-waybar/config.json

check_height() {
    expected=$1
    sh "$repo_dir/scripts/waybar/start"
    jq -e --argjson expected "$expected" '
        .height == $expected and .position == "top" and .output == "TEST-1"
    ' "$generated" >/dev/null
    test -s "$XDG_RUNTIME_DIR/dotfiles-waybar/style.css"
}

# Missing preferences, legacy preferences, every supported height and bad input.
check_height 36
printf '%s\n' '{"waybar":{"position":"top"}}' > "$preferences"
check_height 36
height=28
while [ "$height" -le 64 ]; do
    jq -n --argjson height "$height" '{waybar: {height: $height}}' > "$preferences"
    check_height "$height"
    height=$((height + 1))
done
for invalid in null false 0 27 65 -1 30.5 999999999999999999999 '"028"' '"abc"'; do
    jq -n --argjson height "$invalid" '{waybar: {height: $height}}' > "$preferences"
    check_height 36
done
printf '%s\n' '{invalid json' > "$preferences"
check_height 36

printf '%s\n' '{"waybar":{"height":48,"position":"bottom","primary_only":false}}' > "$preferences"
sh "$repo_dir/scripts/waybar/start"
jq -e '.height == 48 and .position == "bottom" and (has("output") | not)' "$generated" >/dev/null
printf '%s\n' '{"waybar":{"enabled":false}}' > "$preferences"
rm -f "$test_dir/launched"
sh "$repo_dir/scripts/waybar/start"
test ! -f "$test_dir/launched"

# Drive the actual TUI through Waybar > Height, including a leading-zero input.
for value in 28 048 64 27 65 999999999999999999999 -1 30.5 nope cancel; do
    printf '%s\n' '{"waybar":{"height":36,"launcher_icon":"x"}}' > "$preferences"
    rm -f "$test_dir/dialog-count" "$test_dir/restarted"
    WAYBAR_TEST_INPUT=$value sh "$repo_dir/scripts/tui/settings-tui"
    case $value in
        28|048|64)
            jq -e --arg value "$value" '.waybar.height == ($value | tonumber) and .waybar.launcher_icon == "x"' "$preferences" >/dev/null
            test -f "$test_dir/restarted"
            ;;
        *)
            jq -e '.waybar.height == 36' "$preferences" >/dev/null
            test ! -f "$test_dir/restarted"
            ;;
    esac
done
printf '%s\n' 'Waybar startup and settings integration checks passed.'

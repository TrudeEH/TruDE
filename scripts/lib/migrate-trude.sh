#!/bin/sh
# Upgrade the previous installation namespace; active components use TruDE.
# Sourced by install.sh so paths and installer helpers are shared.

migrate_trude_system_paths() (
    set -eu
    migration_root=${1:-}
    for old_path in \
        "$migration_root"/etc/apt/sources.list.d/dotfiles-backports.sources \
        "$migration_root"/etc/apt/sources.list.d/dotfiles-components.sources \
        "$migration_root"/etc/lightdm/lightdm.conf.d/50-dotfiles.conf \
        "$migration_root"/etc/systemd/zram-generator.conf.d/60-dotfiles.conf \
        "$migration_root"/etc/sysctl.d/60-dotfiles-zram.conf \
        "$migration_root"/etc/systemd/logind.conf.d/90-dotfiles-lid.conf; do
        new_path=$(printf '%s\n' "$old_path" | sed 's/dotfiles/TruDE/g')
        [ -e "$old_path" ] || [ -L "$old_path" ] || continue
        if [ -e "$new_path" ] || [ -L "$new_path" ]; then
            backup_dir=$migration_root/etc/TruDE/migration-backups
            sudo install -d -m 0700 "$backup_dir"
            sudo mv -- "$old_path" "$backup_dir/$(basename "$old_path").$(date +%s)-$$"
        else
            sudo mv -- "$old_path" "$new_path"
        fi
    done
)

migrate_trude_user_paths() (
    set -eu
    migration_home=${1:-$HOME}
    data_home=${XDG_DATA_HOME:-$migration_home/.local/share}
    state_home=${XDG_STATE_HOME:-$migration_home/.local/state}
    backup_dir=$state_home/TruDE/migration-backups
    migration_id=$(date +%s)-$$

    # Never overwrite existing settings or chats. Merge missing files if both
    # namespaces exist and retain the old tree as a private conflict backup.
    move_state() {
        old_path=$1
        new_path=$2
        [ -e "$old_path" ] || [ -L "$old_path" ] || return 0
        mkdir -p "$(dirname "$new_path")"
        if [ ! -e "$new_path" ] && [ ! -L "$new_path" ]; then
            mv -- "$old_path" "$new_path"
        else
            if [ -d "$old_path" ] && [ -d "$new_path" ]; then
                cp -an -- "$old_path/." "$new_path/"
            fi
            mkdir -p "$backup_dir"
            chmod 700 "$backup_dir"
            mv -- "$old_path" "$backup_dir/$(basename "$old_path").$migration_id"
        fi
    }

    # Stop old units before changing their executable and storage paths.
    for unit in dotfiles-bar.service dotfiles-dictation.service \
        dotfiles-agent.timer dotfiles-agent.service; do
        if [ -e "$config_dir/systemd/user/$unit" ] || [ -L "$config_dir/systemd/user/$unit" ]; then
            systemctl --user stop "$unit" 2>/dev/null || :
            systemctl --user disable "$unit" 2>/dev/null || :
        fi
    done

    move_state "$config_dir/dotfiles" "$config_dir/TruDE"
    move_state "$config_dir/dotfiles-agent" "$config_dir/TruDE-agent"
    move_state "$data_home/dotfiles" "$data_home/TruDE"
    move_state "$data_home/dotfiles-agent" "$data_home/TruDE-agent"
    move_state "$state_home/dotfiles-agent" "$state_home/TruDE-agent"

    # Only these files contain managed names. Do not rewrite chat history,
    # authentication tokens, or other saved agent content.
    rename_references() {
        path=$1
        [ -f "$path" ] || return 0
        LC_ALL=C grep -q -e dotfiles -e DOTFILES "$path" || return 0
        cp -p -- "$path" "$path.pre-TruDE"
        temporary=$(mktemp "$(dirname "$path")/.TruDE-migration.XXXXXX")
        if ! sed 's/dotfiles/TruDE/g; s/DOTFILES/TRUDE/g' "$path" > "$temporary"; then
            rm -f -- "$temporary"
            return 1
        fi
        chmod --reference="$path" "$temporary"
        mv -- "$temporary" "$path"
    }

    rename_references "$config_dir/hypr/settings.local.lua"
    rename_references "$config_dir/mimeapps.list"
    rename_references "$migration_home/.local/share/applications/mimeapps.list"

    # Preserve wallpaper choices, updating only the old managed default path.
    preferences=$config_dir/TruDE/settings.json
    if [ -f "$preferences" ] && jq -e --arg old "$migration_home/.local/share/backgrounds/dotfiles-wallpaper.jpg" \
        '.appearance.wallpaper == $old' "$preferences" >/dev/null 2>&1; then
        cp -p -- "$preferences" "$preferences.pre-TruDE"
        temporary=$(mktemp "$config_dir/TruDE/.settings.XXXXXX")
        jq --arg wallpaper "$migration_home/.local/share/backgrounds/TruDE-wallpaper.jpg" \
            '.appearance.wallpaper = $wallpaper' "$preferences" > "$temporary"
        chmod --reference="$preferences" "$temporary"
        mv -- "$temporary" "$preferences"
    fi

    # Login entries may be user-created; keep their other keys and choices.
    for entry in "$config_dir"/autostart/*.desktop; do
        rename_references "$entry"
    done
    current_wallpaper=$migration_home/.local/share/backgrounds/current-wallpaper
    if [ -L "$current_wallpaper" ] && \
        [ "$(readlink "$current_wallpaper")" = "$migration_home/.local/share/backgrounds/dotfiles-wallpaper.jpg" ]; then
        ln -sfn -- "$migration_home/.local/share/backgrounds/TruDE-wallpaper.jpg" "$current_wallpaper"
    fi

    # Retire service files and their enablement links, including files written
    # by Seth's scheduler UI. The installer creates the renamed units next.
    for path in "$config_dir"/systemd/user/dotfiles-*.service \
        "$config_dir"/systemd/user/dotfiles-*.timer \
        "$config_dir"/systemd/user/*.target.wants/dotfiles-*; do
        [ -e "$path" ] || [ -L "$path" ] || continue
        mkdir -p "$backup_dir/units-$migration_id"
        chmod 700 "$backup_dir" "$backup_dir/units-$migration_id"
        # Enablement links duplicate the unit's basename; store them separately.
        destination=$backup_dir/units-$migration_id/$(basename "$(dirname "$path")")
        mkdir -p "$destination"
        mv -- "$path" "$destination/$(basename "$path")"
    done

    # Remove only installer-owned links. Preserve unrelated custom files that
    # happen to use the previous prefix by moving them to the new name.
    for path in "$migration_home"/.local/bin/dotfiles-* \
        "$migration_home"/.local/share/applications/dotfiles-*.desktop \
        "$migration_home"/.local/share/backgrounds/dotfiles-wallpaper.jpg \
        "$config_dir"/btop/themes/dotfiles.theme \
        "$config_dir"/qt6ct/colors/dotfiles.conf \
        "$config_dir"/micro/colorschemes/dotfiles.micro \
        "$config_dir"/superfile/theme/dotfiles.toml; do
        [ -e "$path" ] || [ -L "$path" ] || continue
        if [ -L "$path" ]; then
            source=$(readlink "$path")
            case $source in
                "$repo_dir"/*) rm -- "$path"; continue ;;
            esac
        fi
        new_name=$(basename "$path" | sed 's/dotfiles/TruDE/g')
        move_state "$path" "$(dirname "$path")/$new_name"
    done
)

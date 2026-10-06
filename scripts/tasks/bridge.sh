#!/bin/sh
set -eu
base=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
. "$base/backend.sh"
action=${1:?}
shift
tasks_config_init
case $action in
    preference)
        if [ -f "$tasks_config/backend" ]; then cat "$tasks_config/backend"; fi
        exit ;;
    configure)
        case $1 in local|todoist) ;; *) exit 1 ;; esac
        tasks_remember_backend "$1"
        exit ;;
    token)
        token_tmp=$(mktemp "$tasks_config/.token.XXXXXX")
        trap 'rm -f "$token_tmp"' EXIT
        cat > "$token_tmp"
        [ -s "$token_tmp" ] || exit 1
        chmod 600 "$token_tmp"
        mv "$token_tmp" "$tasks_config/token"
        exit ;;
esac
TASKS_BACKEND=${1:?}
shift
tasks_init
case $action in
    snapshot)
        projects_list > "$tasks_tmp/projects"
        tasks_list > "$tasks_tmp/tasks"
        jq -r '.[] | ["P",.id,.name] | @tsv' "$tasks_tmp/projects"
        jq -r 'sort_by(.is_completed // false, -(.priority // 1), .content)[] |
            ["T",.id,.project_id,.content,(.description // ""),
             (.due_string // .due.string // .due.date // ""),
             ((.labels // []) | join(",")),(5-(.priority // 1)),
             (if .is_completed then "1" else "0" end),(.due.date // .due_string // "")] | @tsv' "$tasks_tmp/tasks" ;;
    save)
        id=$1; shift
        payload=$(jq -nc --arg content "$1" --arg description "$2" --arg project "$3" \
            --arg due "$4" --arg labels "$5" --arg priority "$6" --arg dirty "$7" \
            '{content:$content,description:$description,project_id:$project,priority:(5-($priority|tonumber)),
              labels:($labels|split(",")|map(gsub("^ +| +$";"")|select(length>0)))} +
              (if $dirty == "1" then {due_string:$due} else {} end)')
        task_save "$id" "$payload" ;;
    complete) task_complete "$1" "$2" ;;
    delete) task_delete "$1" ;;
    project-save) project_save "$1" "$2" ;;
    project-delete) project_delete "$1" ;;
    *) exit 1 ;;
esac

#!/bin/sh
# Sourced by Tasks. All mutations take JSON, never evaluated shell text.
tasks_config_init() {
    umask 077
    tasks_config=${XDG_CONFIG_HOME:-"$HOME/.config"}/tasks
    mkdir -p "$tasks_config"
}
tasks_remember_backend() {
    config_tmp=$(mktemp "$tasks_config/.backend.XXXXXX") || return 1
    printf '%s\n' "$1" > "$config_tmp"
    mv "$config_tmp" "$tasks_config/backend"
}
# Run in a subshell so signal handlers always restore terminal echo.
tasks_prompt_token() (
    token_terminal=$(stty -g < /dev/tty) || exit 1
    token_tmp=
    trap 'stty "$token_terminal" < /dev/tty; [ -z "$token_tmp" ] || rm -f "$token_tmp"' EXIT
    trap 'exit 130' INT
    trap 'exit 1' HUP TERM
    printf '\nTodoist API token (Settings → Integrations → Developer).\nSaved privately in %s/token. Input is hidden; Enter cancels: ' "$tasks_config" > /dev/tty
    stty -echo < /dev/tty
    IFS= read -r token_value < /dev/tty || exit 1
    stty "$token_terminal" < /dev/tty
    printf '\n' > /dev/tty
    [ -n "$token_value" ] || exit 1
    case $token_value in *"$(printf '\r')"*) exit 1 ;; esac
    token_tmp=$(mktemp "$tasks_config/.token.XXXXXX") || exit 1
    printf '%s\n' "$token_value" > "$token_tmp"
    mv "$token_tmp" "$tasks_config/token"
    token_tmp=
)
tasks_init() {
    umask 077
    tasks_home=${XDG_DATA_HOME:-"$HOME/.local/share"}/tasks
    mkdir -p "$tasks_home"
    tasks_db=$tasks_home/local.json
    tasks_backend=${TASKS_BACKEND:-local}
    case $tasks_backend in local|todoist) ;; *) printf 'Unknown backend: %s\n' "$tasks_backend" >&2; return 1 ;; esac
    tasks_tmp=$(mktemp -d "$tasks_home/.session.XXXXXX")
    trap 'rm -rf "$tasks_tmp"' EXIT
    trap 'exit 130' INT
    trap 'exit 1' HUP TERM
    if [ "$tasks_backend" = local ]; then
        (
            flock -x 9
            if [ ! -e "$tasks_db" ]; then
                printf '%s\n' '{"projects":[{"id":"inbox","name":"Inbox"}],"tasks":[]}' > "$tasks_tmp/new"
                mv "$tasks_tmp/new" "$tasks_db"
            fi
            jq -e '.projects | type == "array"' "$tasks_db" >/dev/null
            jq -e '.tasks | type == "array"' "$tasks_db" >/dev/null
        ) 9>"$tasks_home/local.lock" || return 1
    else
        tasks_token=${TODOIST_API_TOKEN:-}
        if [ -z "$tasks_token" ] && [ -f "${XDG_CONFIG_HOME:-"$HOME/.config"}/tasks/token" ]; then
            tasks_token=$(cat "${XDG_CONFIG_HOME:-"$HOME/.config"}/tasks/token")
        fi
        if [ -z "$tasks_token" ]; then
            printf 'Connect Todoist from the Tasks backend menu, or set TODOIST_API_TOKEN.\n' >&2
            return 1
        fi
        case $tasks_token in *"
"*|*"$(printf '\r')"*) printf 'Invalid API token.\n' >&2; return 1 ;; esac
        printf 'Authorization: Bearer %s\n' "$tasks_token" > "$tasks_tmp/auth"
        unset tasks_token
    fi
}

# HTTPS only, bounded requests, no automatic mutation retries or token in argv.
api() {
    api_method=$1 api_path=$2 api_payload=${3:-}
    case $api_path in *[!a-zA-Z0-9_/?=.%\&-]*) printf 'Invalid API path.\n' >&2; return 1 ;; esac
    if [ -n "$api_payload" ]; then
        printf '%s' "$api_payload" > "$tasks_tmp/request"
        api_code=$(curl --silent --show-error --connect-timeout 10 --max-time 30 \
            --proto '=https' --request "$api_method" --header "@$tasks_tmp/auth" \
            --header 'Content-Type: application/json' --data-binary "@$tasks_tmp/request" \
            --output "$tasks_tmp/response" --write-out '%{http_code}' \
            "https://api.todoist.com/api/v1/$api_path") || return 1
    else
        api_code=$(curl --silent --show-error --connect-timeout 10 --max-time 30 \
            --proto '=https' --request "$api_method" --header "@$tasks_tmp/auth" \
            --output "$tasks_tmp/response" --write-out '%{http_code}' \
            "https://api.todoist.com/api/v1/$api_path") || return 1
    fi
    case $api_code in
        2??) if [ -s "$tasks_tmp/response" ]; then cat "$tasks_tmp/response"; else printf 'null\n'; fi ;;
        *) printf 'Todoist HTTP %s: %s\n' "$api_code" "$(head -c 500 "$tasks_tmp/response" | tr '\r\n\033' '   ')" >&2
           printf 'If a write timed out, refresh before retrying to avoid duplicates.\n' >&2; return 1 ;;
    esac
}
api_list() {
    list_path=$1 list_cursor=
    printf '[]\n' > "$tasks_tmp/list"
    while :; do
        api GET "$list_path?limit=200${list_cursor:+&cursor=$list_cursor}" > "$tasks_tmp/page" || return 1
        jq -e '.results | type == "array"' "$tasks_tmp/page" >/dev/null || return 1
        jq -s '.[0] + .[1].results' "$tasks_tmp/list" "$tasks_tmp/page" > "$tasks_tmp/merged" || return 1
        mv "$tasks_tmp/merged" "$tasks_tmp/list"
        list_cursor=$(jq -r '.next_cursor // empty | @uri' "$tasks_tmp/page")
        [ -n "$list_cursor" ] || break
    done
    cat "$tasks_tmp/list"
}
local_change() {
    change_filter=$1 change_json=$2 change_id=${3:-}
    (
        flock -x 9
        jq --arg id "$change_id" --argjson item "$change_json" "$change_filter" "$tasks_db" > "$tasks_tmp/new" || exit 1
        mv "$tasks_tmp/new" "$tasks_db"
    ) 9>"$tasks_home/local.lock"
}
new_id() { od -An -N16 -tx1 /dev/urandom | tr -d ' \n'; }
validate_task() {
    printf '%s' "$1" | jq -e '
      type == "object" and (.content | type == "string" and length > 0) and
      ((.description // "") | type == "string") and
      ((.priority // 1) | type == "number" and . >= 1 and . <= 4 and floor == .) and
      ((.labels // []) | type == "array" and all(.[]; type == "string")) and
      ((.project_id // "") | type == "string") and
      ((.due_string // "") | type == "string")' >/dev/null
}
tasks_list() {
    if [ "$tasks_backend" = local ]; then jq '.tasks' "$tasks_db"; else api_list tasks; fi
}
projects_list() {
    if [ "$tasks_backend" = local ]; then jq '.projects' "$tasks_db"; else api_list projects; fi
}
task_save() {
    save_id=$1 save_json=$2
    validate_task "$save_json" || { printf 'Invalid task fields.\n' >&2; return 1; }
    if [ "$tasks_backend" = todoist ]; then
        save_json=$(printf '%s' "$save_json" | jq 'if .due_string == "" then .due_string = "no date" else . end')
        if [ -n "$save_id" ]; then
            # Moving uses the dedicated v1 endpoint, not the update endpoint.
            move_project=$(printf '%s' "$save_json" | jq -r '.project_id // empty')
            save_json=$(printf '%s' "$save_json" | jq 'del(.project_id)')
            api POST "tasks/$save_id" "$save_json" >/dev/null || return 1
            if [ -n "$move_project" ]; then
                api POST "tasks/$save_id/move" "$(jq -nc --arg p "$move_project" '{project_id:$p}')" >/dev/null || return 1
            fi
        else api POST tasks "$save_json" >/dev/null; fi
    else
        # Local dates are literal; recurrence belongs to Todoist.
        local_change '
          if any(.projects[]; .id == $item.project_id) then
            if $id == "" then .tasks += [$item + {id:$item._id, is_completed:false} | del(._id)]
            elif any(.tasks[]; .id == $id) then .tasks |= map(if .id == $id then . + $item | del(._id) else . end)
            else error("Task not found") end
          else error("Project not found") end' \
          "$(printf '%s' "$save_json" | jq --arg id "$(new_id)" '. + {_id:$id} | .project_id //= "inbox"')" "$save_id"
    fi
}
task_complete() {
    complete_id=$1 complete_state=$2
    if [ "$tasks_backend" = todoist ]; then
        if [ "$complete_state" = true ]; then complete_action=close; else complete_action=reopen; fi
        api POST "tasks/$complete_id/$complete_action" >/dev/null
    else
        local_change 'if any(.tasks[]; .id == $id) then .tasks |= map(if .id == $id then .is_completed = $item else . end) else error("Task not found") end' "$complete_state" "$complete_id"
    fi
}
task_delete() {
    if [ "$tasks_backend" = todoist ]; then api DELETE "tasks/$1" >/dev/null
    else local_change '.tasks |= map(select(.id != $id))' null "$1"; fi
}
project_save() {
    project_id=$1 project_name=$2
    [ -n "$project_name" ] || return 1
    project_json=$(jq -nc --arg name "$project_name" '{name:$name}')
    if [ "$tasks_backend" = todoist ]; then
        api POST "projects${project_id:+/$project_id}" "$project_json" >/dev/null
    else
        local_change 'if $id == "" then .projects += [$item] elif any(.projects[]; .id == $id) then .projects |= map(if .id == $id then .name = $item.name else . end) else error("Project not found") end' \
            "$(printf '%s' "$project_json" | jq --arg id "$(new_id)" '. + {id:$id}')" "$project_id"
    fi
}
project_delete() {
    if [ "$tasks_backend" = todoist ]; then api DELETE "projects/$1" >/dev/null
    else
        [ "$1" != inbox ] || { printf 'Cannot delete Inbox.\n' >&2; return 1; }
        local_change '.projects |= map(select(.id != $id)) | .tasks |= map(if .project_id == $id then .project_id = "inbox" else . end)' null "$1"
    fi
}

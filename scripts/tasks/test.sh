#!/bin/sh
set -eu
base=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT
export XDG_DATA_HOME=$temporary XDG_CONFIG_HOME=$temporary/config TASKS_BACKEND=local
app=$base/tui/tasks-tui
"$app" projects | jq -e '.[0].id == "inbox"' >/dev/null
"$app" add '{"content":"Unicode café 日本語","description":"$(touch /tmp/not-run)","project_id":"inbox","priority":4,"labels":["work"],"due_string":"2026-10-08"}'
id=$("$app" list | jq -r '.[0].id')
"$app" edit "$id" '{"content":"Edited","project_id":"inbox","priority":2}'
"$app" list | jq -e 'length == 1 and .[0].content == "Edited" and .[0].labels == ["work"] and .[0].due_string == "2026-10-08"' >/dev/null
"$app" complete "$id"
"$app" list | jq -e '.[0].is_completed' >/dev/null
"$app" reopen "$id"
"$app" list | jq -e '.[0].is_completed == false' >/dev/null
if "$app" add '{"content":"Invalid","priority":9}' 2>/dev/null; then exit 1; fi
if "$app" add '{"content":"Invalid","project_id":"missing"}' 2>/dev/null; then exit 1; fi
"$app" project-add 'Work'
project=$("$app" projects | jq -r '.[] | select(.name == "Work") | .id')
"$app" edit "$id" "$(jq -nc --arg p "$project" '{content:"Moved",project_id:$p}')"
"$app" list | jq -e --arg p "$project" '.[0].project_id == $p' >/dev/null
# Exercise library-only deletion and renaming without bypassing TUI confirmation.
(
    . "$base/tasks/backend.sh"
    tasks_init
    project_save "$project" 'Renamed'
    project_delete "$project"
    if project_delete inbox 2>/dev/null; then exit 1; fi
)
"$app" list | jq -e '.[0].project_id == "inbox"' >/dev/null
pids=
for number in 1 2 3 4 5 6 7 8; do
    "$app" add "$(jq -nc --arg n "$number" '{content:$n,project_id:"inbox"}')" &
    pids="$pids $!"
done
for pid in $pids; do wait "$pid"; done
"$app" list | jq -e 'length == 9 and ([.[].id] | unique | length == 9)' >/dev/null
(
    . "$base/tasks/backend.sh"
    tasks_init
    task_delete "$id"
)
"$app" list | jq -e 'length == 8' >/dev/null
[ "$(stat -c '%a' "$temporary/tasks/local.json")" = 600 ]
[ -z "$(find "$temporary/tasks" -maxdepth 1 -name '.session.*' -print)" ]
printf '%s\n' 'PASS: local CRUD, projects, completion, validation, concurrency, permissions, cleanup'

(
    . "$base/tasks/backend.sh"
    tasks_config_init
    tasks_remember_backend todoist
    [ "$(cat "$tasks_config/backend")" = todoist ]
    [ "$(stat -c '%a' "$tasks_config/backend")" = 600 ]
    printf '%s\n' test-token > "$tasks_config/token"
    unset TODOIST_API_TOKEN
    TASKS_BACKEND=todoist
    tasks_init
    grep -qx 'Authorization: Bearer test-token' "$tasks_tmp/auth"
)
printf '%s\n' 'PASS: saved backend and saved Todoist token loading'

${CC:-cc} -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror "$base/tasks/filter-test.c" -o "$temporary/filter-test"
"$temporary/filter-test"

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
"$app" project-rename "$project" Renamed
"$app" project-delete "$project"
if "$app" project-delete inbox 2>/dev/null; then exit 1; fi
"$app" list | jq -e '.[0].project_id == "inbox"' >/dev/null
pids=
for number in 1 2 3 4 5 6 7 8; do
    "$app" add "$(jq -nc --arg n "$number" '{content:$n,project_id:"inbox"}')" &
    pids="$pids $!"
done
for pid in $pids; do wait "$pid"; done
"$app" list | jq -e 'length == 9 and ([.[].id] | unique | length == 9)' >/dev/null
"$app" delete "$id"
"$app" list | jq -e 'length == 8' >/dev/null
[ "$(stat -c '%a' "$temporary/tasks/local.json")" = 600 ]
[ -z "$(find "$temporary/tasks" -maxdepth 1 -name '.session.*' -print)" ]
before=$(stat -c '%i:%Y:%s' "$temporary/tasks/local.json")
"$app" list >/dev/null
[ "$before" = "$(stat -c '%i:%Y:%s' "$temporary/tasks/local.json")" ]
printf '%s' '{broken database' > "$temporary/tasks/local.json"
if "$app" list 2>/dev/null; then exit 1; fi
[ "$(cat "$temporary/tasks/local.json")" = '{broken database' ]
# Remove only this test fixture; native tests below create their own tasks.
rm "$temporary/tasks/local.json"
printf '%s\n' 'PASS: local CRUD, projects, completion, validation, concurrency, permissions, read-only list, corruption safety'

${CC:-cc} -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror \
    ${TASKS_CFLAGS:-} "$base/tasks/filter-test.c" "$base/tasks/backend.c" "$base/tasks/json.c" \
    ${TASKS_LIBS:--lcurl} -lm -o "$temporary/filter-test"
"$temporary/filter-test"
${CC:-cc} -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror \
    ${TASKS_CFLAGS:-} "$base/tasks/backend-test.c" "$base/tasks/backend.c" "$base/tasks/json.c" \
    ${TASKS_LIBS:--lcurl} -lm -o "$temporary/backend-test"
"$temporary/backend-test"

# Tasks

Open **Tasks** from the application launcher after installing the dotfiles.
The first launch offers Local or Todoist; subsequent launches remember your
choice. Press `b` to switch backends or reconnect Todoist. Select Local or
Todoist with arrows/Tab and Enter, or click the option; Escape cancels. No
backend names need to be typed. No arguments needed.
You can also run `scripts/tui/tasks-tui` or `tasks`.
The interface is a native C application, not fzf. It has a persistent project
sidebar, task list, and details panel. Orange pane borders indicate keyboard
focus; inactive panes have muted borders. Dialogs are centered, opaque, and
bordered so underlying tasks cannot bleed into their fields. Runtime dependencies: POSIX sh, jq, curl,
flock (Debian util-linux). Building requires a C compiler (`build-essential` on
Debian); the installer builds it, and the launcher rebuilds changed source.
Run `sh scripts/tasks/build.sh` to rebuild manually.

Local mode is the default. Data lives in
`${XDG_DATA_HOME:-$HOME/.local/share}/tasks/local.json`. Writes use an exclusive
lock and atomic replacement; files are private to the current user. Back up
this file to preserve local tasks. Local and Todoist data are separate; switching
backends does not migrate or upload anything.

Select Todoist in the app and paste your API token at the hidden-input prompt.
Obtain a token from Todoist Settings → Integrations → Developer. Enter without
a token uses the saved token. Escape cancels connection setup. The backend preference and token are
stored under `${XDG_CONFIG_HOME:-$HOME/.config}/tasks/`; the token file is mode
0600. It is plaintext: keep it private and exclude it from shared backups.
Use `b`, select Todoist, and paste a new token to replace it. Failed connections
show an error in the status bar; use `r` to retry or `b` to change backend. CLI users may still set `TODOIST_API_TOKEN`.
Do not commit tokens. Requests use the HTTPS API v1. Tokens are held in private
session files and never put in curl's arguments. Requests have a 30-second
limit. Writes are not automatically retried: refresh after a timeout before
retrying, because the server may already have accepted the write.

## Sidebar views

**Tasks** contains All Tasks, Today (including overdue), Upcoming (dates after
today), and Completed (completed tasks only). **Projects** contains only the
project list; selecting a project shows its active tasks. Arrow navigation and
mouse selection work across both sections.

Today and Upcoming use Todoist's structured due date, not its natural-language
schedule text. Local tasks must use an ISO date (`YYYY-MM-DD`) to appear in
those views; undated tasks remain in All Tasks and their project. Dates use the
computer's local calendar day. Completed works with local data; Todoist's
completed history is not loaded, and its empty view says so explicitly.

## Navigation

- `Tab`: switch between projects and tasks; arrows navigate the focused pane.
- Click selects projects/tasks; wheel scrolls; click a task checkbox to complete.
- Details stay visible below the list.
- `a`: add; `e` or Enter: edit; Space: complete/reopen.
- `n`: new project; F2: rename the selected project.
- `d`: delete the selected task or focused project, with confirmation.
- `/`: search titles, descriptions and labels; Escape clears search.
- `r`: refresh; `b`: backend/account settings; `q`: quit.
- `v`: toggle completed tasks in local mode.

The editor is one form with all fields visible. Tab or arrows switch fields;
Left/Right chooses a project in the project field. Enter saves, Escape cancels,
and Ctrl-u clears the focused text field. Priorities are P1 highest through P4
lowest. Labels are comma-separated. Local dates are literal text; Todoist accepts
natural-language dates and recurring schedules. Unchanged dates are not sent
back to Todoist, preserving recurrence. Description entry is single-line;
existing multi-line descriptions are preserved unless edited. The minimum
window size is 70 columns by 22 rows.

Deleting local projects moves their tasks to Inbox. Deleting Todoist projects
also deletes their tasks, and requires explicit confirmation. The Todoist view
shows active tasks, not completed history. CLI `reopen ID` supports a known
completed Todoist task ID.

## CLI

```
tasks --local project-add Work
tasks --local projects
tasks --local add '{"content":"Buy milk","project_id":"inbox","priority":1,"labels":["errands"]}'
tasks --local list
tasks --local complete TASK_ID
tasks --local reopen TASK_ID
```

`edit ID JSON` uses the same fields as `add`. Include `content` and the target
`project_id`; omitted fields stay unchanged locally. The TUI supplies all
editable fields. JSON values are data, never shell commands.

Scope: task CRUD, completion/reopening, projects, moving tasks, descriptions,
priorities, dates/recurrence (Todoist), labels, and search. This is not a full
Todoist client: sections, subtasks, comments, attachments, reminders, shared
assignments, saved filters, completed history, and offline Todoist sync are not
implemented. Remote calls are synchronous and bounded; local filtering and
navigation run inside the native UI without subprocesses.

API reference: https://developer.todoist.com/api/v1/
Run `sh scripts/tasks/test.sh` for backend tests and
`python3 scripts/tasks/ui-test.py` for real-terminal keyboard/mouse UI tests.

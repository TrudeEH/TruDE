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
bordered so underlying tasks cannot bleed into their fields.

All application logic is C: UI, CLI, JSON, atomic storage, locking, settings, and
Todoist HTTPS. The backend is called directly, without shell, jq, curl, or flock
subprocesses. HTTPS uses libcurl with certificate verification enabled. Runtime
requires libc, libcurl, libcrypto, and POSIX threads. Building requires `build-essential` and
`libcurl4-openssl-dev` and `libssl-dev` on Debian; the installer builds it, and the thin POSIX sh
launcher rebuilds changed source. Build/launcher scripts are infrastructure,
not the application backend. The JSON parser is reused from Seth's C source.
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
Do not commit tokens. Requests use the HTTPS API v1. Tokens are loaded directly
into memory and never passed through subprocess arguments or session files. Requests have a 3-second connection timeout and a 30-second total limit.
Todoist changes are saved to a private persistent queue before network access;
retries reuse stable request IDs, including a separate ID for the move phase of
a task edit. Do not submit an edit again because of a network timeout: it is
already queued.

## Todoist cache and offline sync

Todoist opens immediately from the last cached snapshot. `Loading...` in the
header indicates background sync; navigation and editing remain available.
The app syncs on startup, after edits, on `r`, and every 30 seconds while open.
When offline, cached tasks/projects remain usable and changes survive closing
the app. On reconnect, the app uploads queued changes in order, then downloads
the current project/task lists. A closed app does not sync; reopen it or run
`tasks --todoist sync` to upload pending changes.

Cache and queue are stored together atomically in mode-0600
`${XDG_DATA_HOME:-$HOME/.local/share}/tasks/todoist-TOKEN_SHA256/cache.json`.
The token fingerprint isolates credentials so a different token never uploads
another token's pending changes. Changing a token creates a separate cache;
changes queued under the old token remain there. Local mode stays separate.
Back up the cache file to preserve unsynced work. Never delete it to retry sync.

An initial online sync is required to obtain Todoist projects. Before that,
there is no remote project list to use offline. CLI `list` and `projects` read
cached data, and CLI mutations queue changes; use `tasks --todoist sync` to
explicitly synchronize. The TUI performs that synchronization automatically.
New tasks/projects have temporary IDs until upload; dependent queued changes
are remapped to the server IDs. Project deletion still deletes its tasks.
Completed tasks disappear after a successful download because the API lists
active tasks only; completed history is not an offline archive.

Pending local fields are sent before fetching fresh server data: they take
precedence over edits to the same fields made elsewhere. Untouched fields are
not deliberately changed. This is not a three-way merge. A rejected operation
(e.g. expired credentials or an object deleted elsewhere) stays queued and
shows an error; later operations wait behind it. There is no queue-resolution
UI yet. Do not delete the cache or switch accounts as a substitute for resolving
such an error. Request IDs reduce duplicate writes after ambiguous timeouts,
but server deduplication is not a promise of indefinite exactly-once delivery.

Run `python3 scripts/tasks/offline-test.py` for isolated local HTTP-server tests
of cached reads, offline changes, retries, ID remapping, account isolation,
corruption safety, and nonblocking cached UI startup. The HTTP endpoint override
exists only in test builds; production remains HTTPS-only. No live Todoist
account was used for these tests.

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
assignments, saved filters, completed history, and conflict-resolution dialogs
are not implemented. Remote calls run in a background C thread; local filtering
and navigation run inside the native UI without subprocesses.

API reference: https://developer.todoist.com/api/v1/
Run `sh scripts/tasks/test.sh` for backend tests and
`python3 scripts/tasks/ui-test.py` for real-terminal keyboard/mouse UI tests.

## Performance comparison

Run `python3 scripts/tasks/benchmark.py --samples 25 --output scripts/tasks/benchmark-results.json`
after building. The harness builds old commit `081fa76` separately and compares
it with the current build using isolated local datasets (0, 100, 1,000 tasks).
Python is needed only for tests/benchmarks, not the application.

The recorded run used Debian, Ryzen 7 7700, GCC 14.2, `-O2`, 25 measured
samples per version/dataset, and one discarded warmup. Versions alternate
order. Both use the same data and a 110×34 pseudo-terminal. CLI times include
the launcher; UI times end when Ready is rendered. Builds, dataset resets, and
memory sampling are outside timed sections. Filesystem cache is warm.

| Median, 1,000 tasks | Old C UI + shell backend | Native C application |
| --- | ---: | ---: |
| CLI list | 17.24 ms | 10.59 ms |
| CLI add | 23.76 ms | 11.72 ms |
| UI startup | 38.06 ms | 11.30 ms |
| UI refresh | 35.46 ms | 4.21 ms |
| Idle RSS | 20.80 MiB | 31.57 MiB |
| Idle PSS | 19.15 MiB | 25.67 MiB |
| Idle private memory | 19.13 MiB | 24.52 MiB |

The new version is faster here, **not lower in idle RAM**. Linked libcurl and
its dependencies remain resident; native JSON parsing/serialization also adds
heap allocations inside the UI. The old backend subprocesses exit before idle
memory is sampled. This is not a peak-memory comparison. RSS includes shared
library pages; PSS apportions shared pages, and private memory excludes them.
Measurements include the application process tree but not a terminal emulator.
Raw results, p95 latency, and smaller datasets are in `benchmark-results.json`.
These are local-mode results on this machine, not Todoist/network benchmarks or
cold-start guarantees. Todoist integration has not been tested against a live
account during this migration. The recorded benchmark predates the offline-cache/thread
implementation and is historical, not a measurement of the current build.

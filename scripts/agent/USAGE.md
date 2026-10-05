# Seth

Seth is a Debian terminal application with a small agent loop and separate MCP servers.
Nothing runs a model locally by itself: start LM Studio, Ollama, 9router, or another
OpenAI-compatible server, then select its endpoint and a model that supports tools.

## Start

From this checkout, run `scripts/agent/setup.sh` once, then `scripts/tui/agent-tui`.
The desktop installer links the application launcher and the `dotfiles-agent-tui`
command. Seth is written entirely in C, with POSIX sh launch/build scripts. The
provided amd64 executable is about 1.2 MB and statically linked. Starting it needs
no compiler, Node.js, npm, Python, ncurses, or downloaded library tree. HTTP/TLS uses
`curl` and the system certificate store, which the Debian installer already includes.
Other Debian CPU architectures can rebuild from source with an existing C compiler;
the installer never adds a compiler or another runtime package.
The installer enables and starts the scheduler timer. When no user service manager
is running during installation, the timer starts with the next user session.

Options: `--workspace DIR`, `--help`, `--check` (connect MCP servers), `--run-due`,
and `--run-task ID`. `--prompt TEXT` runs a request in the terminal without the TUI
(using the configured permission policy; Ask declines changes without a dialog).
Headless runs use the same settings and saved chats.

## Interface and chat features

- Full-screen keyboard/mouse TUI with Chat, MCP servers, Automation,
  Memory and Settings, and a sixth Help tab that opens the chat help pop-up.
  No permanent header or help panels.
- Repository dark/orange palette, scrollable conversation, Markdown headings/bold,
  code blocks, muted tool calls collapsed by default (click a row to expand or
  collapse its arguments and result), and a multiline editor supporting paste,
  arrows, Home/End, Ctrl+A/E/U/K, and Unicode.
- Chat speakers show your Linux username with its first letter capitalized and `Seth`,
  including streamed replies.
  Tool expansion stays in place during replies and view switches. Saved history
  and exports always keep complete tool arguments and results.
- Persistent history pane; text search, resume, rename, delete, branch, and export.
- Streaming replies, stop/cancellation, continue, retry in a new branch, request
  timeouts, bounded tool loops, activity logs, and context/usage tracking.
- Automatic context compaction plus manual compaction. Full transcripts remain saved;
  summaries replace only older context sent to the model. Tool call/result pairs
  remain together. Compaction is chunked to fit small context windows.
- Workspace text attachments through the filesystem MCP server.
- Workspace `AGENTS.md` instructions, loaded through MCP before each agent request.
- Ask, read-only, and automatic tool permission modes. Ask is the default. External
  tools require approval even when a server claims they are read-only. File edit
  approval shows current/proposed text. Changing to automatic mode requires confirmation;
  the choice is saved across chats and restarts until changed in Settings. The picker
  highlights the current mode when reopened.
- Atomic private storage, per-chat/process locks, and interrupted tool-call recovery.

Enter sends a message; Shift+Enter adds a line; Ctrl+S also sends. Multiline paste
does not send. Foot's enhanced keyboard reporting distinguishes Shift+Enter without
terminal configuration changes. Ctrl+J is another way to insert a newline.
Escape stops or dismisses a dialog. F1–F5 select Chat, MCP servers, Automation,
Memory, and Settings; F6 or the sixth tab opens chat
help. `?` toggles help outside text fields or in an empty composer; inside a message
it remains a question mark. Tab/Shift+Tab move focus; Ctrl+N creates a chat; Ctrl+L
focuses the composer; Ctrl+Q exits. Dialogs save with Ctrl+S; Enter adds lines in
multiline settings fields.

Commands: `/new`, `/fork`, `/continue`, `/retry`, `/compact`, `/attach PATH`,
`/workspace`, `/export`, `/events`, and `/help`. `/workspace` opens a directory field
for the current chat; the clickable workspace path to the right of Export in Actions opens
the same dialog. The workspace is saved with the chat and restored when reopened.
Settings' default workspace applies to new chats without changing existing chats.
Retry and branch preserve previous transcripts;
retry does not undo any prior tool side effects.

## Provider settings

Presets: LM Studio (`http://127.0.0.1:1234/v1`), Ollama
(`http://127.0.0.1:11434/v1`), 9router (`http://127.0.0.1:20128/v1`), and Custom.
Each keeps its endpoint, model, API key, and API key environment variable independently.
Models can be discovered from `/models` or entered manually. A test-connection action
shows available models. Generation uses `/chat/completions`, accepting SSE streaming
or a JSON response. Set the context window to the actual server/model configuration.

Also configurable: workspace, system instructions, context window, output token limit,
maximum tool rounds, request timeout, and permission mode. API keys are optional for
servers that do not require them. Environment variables take precedence over saved keys.

## MCP servers

The native MCP client implements the MCP v2 / 2026-07-28 wire protocol directly:
`server/discover`, per-request version/client/capability envelopes, modern result
types, in-band `input_required` responses, roots, typed form elicitation, and tool-list
subscriptions. It has no SDK or third-party library dependency. Bundled servers use
the 2026 protocol and reject legacy openings. Compatibility was checked against
the official MCP v2 client as well as isolated stdio/HTTP fixtures.
External servers negotiate automatically; legacy servers remain usable and are
identified in their status. Transports: stdio, Streamable HTTP, and explicit legacy SSE.

Add a local program with one argument per line, or a remote endpoint with an optional
bearer-token environment variable. Advanced editing and import accept the familiar
`{"mcpServers": {...}}` JSON structure. Command arguments, environment values, and
headers may contain `${ENV_VAR}` references. Programs launch directly, without shell
interpolation. Enable/disable, remove, reconnect, inspect tools/errors, and disable
individual tools in the MCP tab. Tool-list changes refresh automatically when advertised.
MCP resources and prompts are exposed to the agent through bridges to the standard MCP
methods. Form elicitation can ask the user for input; URL requests display a sign-in link.

Bundled defaults:

- `filesystem`: list/read/search files, inspect metadata, write/edit text, and list/restore
  checkpoints. File tools reject paths and symlinks outside
  the workspace. Editing text files up to 2 MB creates a private restore checkpoint.
  Recursive search skips symlinks, `.git`, and `node_modules`.
- `web`: free DuckDuckGo search (titles, source links, snippets) and text-page fetching.
  No API key or paid search subscription. Public search can rate limit or challenge
  requests; replace or add a search MCP server when needed.
- `shell`: run `/bin/sh` commands in the selected workspace, with a bounded timeout
  and output. It can be enabled or disabled independently of filesystem tools.
- `memory`: `store_memory`, `read_memory`, `search_memories`, `list_memories`, and
  `delete_memory`. Store a title and content; supply the returned ID to update,
  retrieve, or delete that memory. Literal search ignores case. Memories persist
  across chats and workspaces in private `memories.json` under the agent's XDG data
  directory. Reads are permitted in read-only mode; saving and deleting follow the
  normal tool approval rules. Concurrent server processes use a lock and atomic saves.

The Memory tab lists saved memories and shows the selected memory's full content.
New creates a memory with a title and multiline content.
Click a row or use the arrow keys to select a memory; Read opens the complete text,
Edit opens its title and multiline content, Delete asks for confirmation, and Refresh
reloads the list. Manual changes are user actions and work in read-only agent mode.
Edits and deletes detect changes made while the dialog was open. Enable the bundled
memory server and the corresponding tools to use this tab.

Function-key navigation accepts legacy SS3, CSI, Linux console, and modern keyboard
reporting sequences. The enhanced keyboard encodings follow the
[terminal keyboard protocol](https://sw.kovidgoyal.net/kitty/keyboard-protocol/).

Existing version 1 settings migrate once to version 2, adding memory and the separate
shell server. A disabled filesystem server or shell tool keeps shell disabled. Explicit
shell grants in schedules now refer to the shell server's tool; select it in Allowed tools.

Shell commands and external MCP processes have the user's OS permissions. Workspace
checks apply to the bundled file tools; they are not a sandbox for arbitrary programs.
Sampling is not advertised. Automatic OAuth/browser authorization, binary/image input,
and MCP Apps rendering are not implemented; remote authorization uses configured
headers/environment variables or explicit user sign-in.

## Automation

Native tools let the model list, create, update, and delete schedules (changes require
approval). Scheduling belongs to the agent rather than an MCP server.

The Automation tab supports creation/editing, pause/resume, deletion, manual runs,
opening the latest result chat, and enabling/disabling the background timer. It shows
instructions, cron, timezone, next/last run, model, workspace, tool permissions, status,
and errors, and refreshes when the task file changes.

Schedules use five cron fields (`minute hour day month weekday`) and an IANA timezone.
For example, `0 9 * * 1-5` means 09:00 on weekdays in the selected timezone.
The Debian systemd user timer checks due tasks each minute. Missed executions coalesce
into one run. Tasks cannot overlap themselves; each run creates a separate history chat.
The timer works while the TUI is closed and the user's systemd session is running.
Running while logged out requires a user manager that remains active (systemd linger).

Tasks remember workspace, provider profile, model, and an explicit allowed-tool list.
Bundled read tools are allowed by default. Select extra tools in the task editor to
allow changes or external tools without prompts. Unapproved actions fail visibly;
background execution never opens an approval dialog or creates more schedules.
Settings changes take effect at the next run; credentials are not copied into tasks.
API-key environment variables must also exist in the systemd user service environment.

## Storage and code layout

The native port reads the existing settings, chat and schedule JSON formats directly.
It also preserves hashed tool aliases used in old chats and automation permissions.
No migration, history conversion, or fresh setup is needed. Previous npm runtime
folders are unused; neither startup nor setup reads or downloads them.

Uses XDG directories, defaulting to:

- `~/.config/dotfiles-agent/settings.json`: provider and MCP settings (0600).
- `~/.local/share/dotfiles-agent/chats/`: complete chat JSON files (0600).
- `~/.local/share/dotfiles-agent/automations.json`: schedules and last results.
- `~/.local/state/dotfiles-agent/checkpoints/`: private file restore checkpoints.
- `~/.local/state/dotfiles-agent/*.lock`: live-process concurrency guards.

| File | Purpose |
| --- | --- |
| `../tui/agent-tui` | POSIX sh launcher; chooses the native CPU binary |
| `bin/seth-amd64`, `bin/SHA256SUMS` | Static native executable and release checksum |
| `src/main.c` | Interactive/headless dispatch and bundled server modes |
| `src/tui.c` | Terminal rendering, tabs, mouse/keyboard editor, dialogs and history |
| `src/agent.c` | Model/tool loop, permissions, context compaction and cancellation |
| `src/provider.c` | OpenAI-compatible generation, model discovery and SSE parsing |
| `src/mcp.c` | MCP v2 connections, legacy negotiation, resources/prompts and subscriptions |
| `src/servers/server.c`, `src/servers/servers.h` | Shared bundled stdio MCP dispatch and tool helpers |
| `src/servers/filesystem.c` | Workspace files, search, edits and checkpoints |
| `src/servers/web.c` | Free DuckDuckGo search and source-page text extraction |
| `src/servers/shell.c` | Independent workspace shell commands |
| `src/servers/memory.c` | Persistent memory storage, lookup and search |
| `src/automation.c` | Native cron/timezone handling, schedule tools and background execution |
| `src/store.c` | Settings, private atomic JSON storage, history and process locks |
| `src/net.c`, `src/util.c` | Bounded curl/process pipes, paths, UUIDs and stable SHA-256 aliases |
| `src/json.c`, `src/schema.c` | Bounded JSON parser and bundled tool/form validation |
| `build.sh`, `setup.sh` | Optional native rebuild and dependency-free setup |
| `src/tests.c`, `test/native.py`, `test.sh` | Native checks and development-only integration fixtures |

Desktop integration lives in `configs/applications/dotfiles-agent.desktop`, the two
`configs/systemd/user/dotfiles-agent.*` units, `configs/hypr/hyprland.lua`, and `install.sh`.

Run `scripts/agent/test.sh` to rebuild and verify with a C compiler. The native
self-tests need no other test runtime. If Python 3 is already available, the script
also runs standard-library integration tests with isolated model/MCP endpoints and
pseudo-terminal interactions. Python is only a development test helper; the agent,
installer, scheduler, and bundled servers never invoke it. Tests use temporary XDG
directories and do not contact a real model server or change user settings.

To rebuild the packaged executable, run `scripts/agent/build.sh`. It uses only the
compiler, standard Linux libc headers/archive, and the linker; no dependency fetch
or package manager is involved. `CC` selects a compiler. Native server processes
use the same executable with `--mcp-filesystem`, `--mcp-web`, `--mcp-shell`, or
`--mcp-memory`; all their actions
remain MCP requests rather than being moved into the agent loop.

# Seth

Seth is a Debian terminal application with a small agent loop and separate MCP servers.
Nothing runs a model locally by itself: start LM Studio, Ollama, 9router, or another
OpenAI-compatible server, then select its endpoint and a model that supports tools.

## Start

From this checkout, run `scripts/agent/setup.sh` once, then `scripts/tui/agent-tui`.
The desktop installer installs Node.js/npm, the locked dependencies, an application
launcher, and the `dotfiles-agent-tui` command. Node.js 20 or newer is required.
The scheduler timer is installed but stays disabled until enabled in the TUI.

Options: `--workspace DIR`, `--help`, `--check` (connect MCP servers), `--run-due`,
and `--run-task ID`. Headless runs use the same settings and saved chats.

## Interface and chat features

- Full-screen keyboard/mouse TUI with Chat, MCP servers, Automation, Settings,
  and a fifth Help tab that opens the chat help pop-up. No permanent header or help panels.
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
Escape stops or dismisses a dialog. F1–F4 select tabs; F5 or the fifth tab opens chat
help. `?` toggles help outside text fields or in an empty composer; inside a message
it remains a question mark. Tab/Shift+Tab move focus; Ctrl+N creates a chat; Ctrl+L
focuses the composer; Ctrl+Q exits. Dialogs save with Ctrl+S; Enter adds lines in
multiline settings fields.

Commands: `/new`, `/fork`, `/continue`, `/retry`, `/compact`, `/attach PATH`,
`/export`, `/events`, and `/help`. Retry and branch preserve previous transcripts;
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

The official MCP SDK v2 (locked version 2.3.0) supplies transport, discovery,
protocol negotiation, schema checking, progress/cancellation, and modern in-band
input handling. Bundled servers use the 2026 protocol and reject legacy openings.
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

- `filesystem`: list/read/search files, inspect metadata, write/edit text, list/restore
  checkpoints, and run `/bin/sh` commands. File tools reject paths and symlinks outside
  the workspace. Editing text files up to 2 MB creates a private restore checkpoint.
  Recursive search skips symlinks, `.git`, and `node_modules`.
- `web`: free DuckDuckGo search (titles, source links, snippets) and text-page fetching.
  No API key or paid search subscription. Public search can rate limit or challenge
  requests; replace or add a search MCP server when needed.

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

Uses XDG directories, defaulting to:

- `~/.config/dotfiles-agent/settings.json`: provider and MCP settings (0600).
- `~/.local/share/dotfiles-agent/chats/`: complete chat JSON files (0600).
- `~/.local/share/dotfiles-agent/automations.json`: schedules and last results.
- `~/.local/share/dotfiles-agent/runtime/`: installed dependency tree.
- `~/.local/state/dotfiles-agent/checkpoints/`: private file restore checkpoints.
- `~/.local/state/dotfiles-agent/*.lock`: live-process concurrency guards.

| File | Purpose |
| --- | --- |
| `../tui/agent-tui` | POSIX sh entry point; resolves checkout and installed runtime |
| `main.cjs` | Interactive/headless command dispatch |
| `tui.cjs` | Tabs, panes, editor, settings forms, approvals, history |
| `terminal.cjs` | Foot compatibility and exact RGB palette output |
| `input.cjs` | Enhanced terminal keys, Shift+Enter, and bracketed paste |
| `agent.cjs` | Small model/tool loop, permissions, compaction, cancellation |
| `provider.cjs` | OpenAI-compatible HTTP and streaming parser |
| `mcp.cjs` | SDK v2 clients, tool registry, resource/prompt bridges |
| `store.cjs` | Defaults, validation, private atomic storage, locks |
| `automation.cjs` | Native schedule tools, cron, background execution |
| `servers/filesystem.cjs` | Separate MCP filesystem/checkpoint/shell server |
| `servers/web.cjs` | Separate MCP free-search/page-fetch server |
| `setup.sh`, `package*.json` | Locked, isolated dependency installation |
| `test/agent.test.cjs` | Integration tests with local mock endpoints and real MCP servers |

Desktop integration lives in `configs/applications/dotfiles-agent.desktop`, the two
`configs/systemd/user/dotfiles-agent.*` units, `configs/hypr/hyprland.lua`, and `install.sh`.

Run verification with `npm test` in `scripts/agent`. Tests use temporary XDG directories
and mock model endpoints; they do not depend on a real model server or change user settings.

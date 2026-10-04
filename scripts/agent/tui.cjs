'use strict';
const blessed = require('blessed');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const { enableInput } = require('./input.cjs');
const { Agent, estimate } = require('./agent.cjs');
const { MCP } = require('./mcp.cjs');
const { Provider } = require('./provider.cjs');
const { Scheduler, nativeTools } = require('./automation.cjs');
const { PROFILES, validateServer, atomic } = require('./store.cjs');
const { COLORS: C, terminalOptions, configureTerminal } = require('./terminal.cjs');
const clean = value => String(value ?? '').replace(/\x1b\[[0-?]*[ -/]*[@-~]/g, '').replace(/[\x00-\x08\x0b-\x1f\x7f-\x9f]/g, '');
const esc = value => blessed.escape(clean(value));
const capitalize = value => value.replace(/^./u, char => char.toUpperCase());
const orange = value => `{${C.accent}-fg}${esc(value)}{/${C.accent}-fg}`;
const muted = value => `{${C.muted}-fg}${esc(value)}{/${C.muted}-fg}`;
const style = { fg: C.text, bg: C.bg, border: { fg: C.muted, bg: C.bg }, focus: { border: { fg: C.accent } } };
function box(parent, options = {}) { return blessed.box({ parent, tags: true, style: structuredClone(style), ...options }); }
function button(parent, label, left, action, options = {}) {
  const widget = blessed.button({ parent, content: label, left, height: 1, width: label.length + 4, mouse: true, keys: true, padding: { left: 2, right: 2 }, style: { fg: C.bg, bg: C.accent, focus: { bg: C.hover }, hover: { bg: C.hover } }, ...options });
  widget.on('press', action); return widget;
}
// Small Unicode-aware editor: arrows, Home/End, multiline paste and ordinary terminal keys.
// It avoids blessed's input grab, so tab navigation and app shortcuts keep working.
class Editor {
  constructor(parent, options = {}) {
    this.value = options.value || ''; this.cursor = Array.from(this.value).length; this.multiline = options.multiline; this.secret = options.secret;
    this.widget = box(parent, { border: 'line', keys: true, mouse: true, scrollable: true, ...options, tags: true });
    this.widget._editor = this; this.onChange = options.onChange;
    this.widget.on('focus', () => this.render()); this.widget.on('blur', () => this.render()); this.widget.on('click', () => this.widget.focus());
    this.widget.on('keypress', (ch, key) => {
      if (key._handled) return;
      let chars = Array.from(this.value), i = this.cursor;
      const before = chars.slice(0, i).join(''), start = before.lastIndexOf('\n') + 1;
      if (key.ctrl) {
        if (key.name === 'a') i = Array.from(before.slice(0, start)).length;
        else if (key.name === 'e') { const end = chars.indexOf('\n', i); i = end < 0 ? chars.length : end; }
        else if (key.name === 'u') { const cut = Array.from(before.slice(0, start)).length; chars.splice(cut, i - cut); i = cut; }
        else if (key.name === 'k') { const end = chars.indexOf('\n', i); chars.splice(i, (end < 0 ? chars.length : end) - i); }
        else return;
      } else if (key.name === 'left') i = Math.max(0, i - 1);
      else if (key.name === 'right') i = Math.min(chars.length, i + 1);
      else if (key.name === 'home') { while (i > 0 && chars[i - 1] !== '\n') i--; }
      else if (key.name === 'end') { while (i < chars.length && chars[i] !== '\n') i++; }
      else if (['up', 'down'].includes(key.name) && this.multiline) {
        const lines = this.value.split('\n'); const row = before.split('\n').length - 1, column = Array.from(before.split('\n').at(-1)).length;
        const target = Math.max(0, Math.min(lines.length - 1, row + (key.name === 'up' ? -1 : 1)));
        i = lines.slice(0, target).reduce((sum, line) => sum + Array.from(line).length + 1, 0) + Math.min(column, Array.from(lines[target]).length);
      } else if (key.name === 'backspace') { if (i) chars.splice(--i, 1); }
      else if (key.name === 'delete') chars.splice(i, 1);
      else if (key.name === 'enter') {
        if (!this.multiline || (options.submitOnEnter && !key.shift)) { options.onSubmit?.(this.value); return; }
        chars.splice(i++, 0, '\n');
      }
      else if (key.name === 'linefeed' && this.multiline) chars.splice(i++, 0, '\n');
      else if (ch && !key.meta && !['tab', 'escape'].includes(key.name)) {
        const text = clean(ch).replace(this.multiline ? /\r/g : /[\r\n]/g, ''); const added = Array.from(text); chars.splice(i, 0, ...added); i += added.length;
      } else return;
      this.value = chars.join(''); this.cursor = i; this.render(); options.onChange?.(this.value);
    });
    this.render();
  }
  set(value) { this.value = value; this.cursor = Array.from(value).length; this.render(); }
  insert(value) {
    const added = Array.from(clean(value).replace(/\r\n?/g, '\n').replace(this.multiline ? /$^/g : /\n/g, ''));
    const chars = Array.from(this.value); chars.splice(this.cursor, 0, ...added); this.cursor += added.length;
    this.value = chars.join(''); this.render(); this.onChange?.(this.value);
  }
  render() {
    if (!this.widget.parent || this.widget.detached) return;
    const chars = Array.from(this.secret ? '•'.repeat(Array.from(this.value).length) : this.value);
    if (this.widget.screen.focused === this.widget) {
      const current = chars[this.cursor] === '\n' ? ' ' : chars[this.cursor] || ' ';
      this.widget.setContent(esc(chars.slice(0, this.cursor).join('')) + `{inverse}${esc(current)}{/inverse}` + esc(chars.slice(this.cursor + (chars[this.cursor] === '\n' ? 0 : 1)).join('')));
    } else this.widget.setContent(esc(chars.join('')) || muted(this.widget.options.placeholder || ''));
    this.widget.setScrollPerc(100); this.widget.screen.render();
  }
}
class TUI {
  constructor(store, config, { screen } = {}) {
    this.store = store; this.config = config; this.scheduler = new Scheduler(store); this.tab = 0; this.busy = false; this.partial = ''; this.notice = ''; this.progress = ''; this.modal = null;
    this.username = process.env.USER?.trim() || process.env.LOGNAME?.trim() || os.userInfo().username;
    this.expandedTools = new Set(); this.toolRows = new Map();
    this.chat = this.fresh();
    this.screen = screen || blessed.screen({ ...terminalOptions(), smartCSR: true, fullUnicode: true, title: 'Seth', dockBorders: true, autoPadding: true, warnings: false });
    configureTerminal(this.screen);
    enableInput(this.screen);
    this.navigation = box(this.screen, { top: 0, height: 3, width: '100%', border: 'line', label: ' Views ' });
    this.tabs = ['Chat', 'MCP servers', 'Automation', 'Settings', 'Help (?)'].map((name, i) => {
      const widget = button(this.navigation, `F${i + 1} ${name}`, 1 + i * 20, () => this.selectTab(i), { top: 0, width: 19 });
      return widget;
    });
    this.body = box(this.screen, { top: 3, bottom: 1, width: '100%' });
    this.status = box(this.screen, { bottom: 0, height: 1, width: '100%', padding: { left: 1 } });
    this.screen.key(['f1', 'f2', 'f3', 'f4'], (_, key) => { if (!this.modal) this.selectTab(Number(key.name.slice(1)) - 1); });
    this.screen.key(['?', 'f5'], (_, key) => {
      if (this.modal && this.modal !== this.helpModal) return;
      // Keep question marks available in text. F5 always opens help; ? also
      // opens it from an empty composer or when focus is outside an editor.
      if (key.name !== 'f5' && this.screen.focused?._editor?.value) return;
      key._handled = true; this.showHelp();
    });
    this.screen.key(['tab', 'S-tab'], (_, key) => {
      const parent = this.modal || this.body;
      const widgets = this.focusables(parent); if (!widgets.length) return;
      let i = widgets.indexOf(this.screen.focused); i = (i + (key.shift ? -1 : 1) + widgets.length) % widgets.length; widgets[i].focus(); this.screen.render();
    });
    this.screen.key('C-n', () => { if (!this.modal && !this.busy) { this.chat = this.fresh(); this.selectTab(0); } });
    this.screen.key('C-s', () => { if (!this.modal && this.tab === 0) this.submit(); });
    this.screen.key('C-l', () => { if (!this.modal && this.tab === 0) this.composer.widget.focus(); });
    this.screen.key('escape', () => { if (this.modal) return; if (this.busy) this.controller?.abort(new Error('Stopped by user.')); else this.screen.focused?.blur(); });
    this.screen.key(['C-q', 'C-c'], () => this.quit());
    this.screen.on('resize', () => this.renderStatus());
    this.selectTab(0);
    this.mcp = new MCP(config, { onStatus: () => { if (this.tab === 1 && !this.modal) this.drawMCP(); this.renderStatus(); }, elicit: (name, params) => this.elicit(name, params) });
    this.ready = this.reconnect();
    let taskStamp = '';
    const refresh = setInterval(() => {
      if (this.tab !== 2 || this.modal || this.busy) return;
      try { const stamp = fs.statSync(this.store.taskFile).mtimeMs; if (stamp !== taskStamp) { taskStamp = stamp; this.drawAutomation(); } } catch {}
    }, 5000);
    refresh.unref(); this.screen.on('destroy', () => clearInterval(refresh));
  }
  fresh() { const p = this.config.profiles[this.config.profile]; return this.store.newChat(this.config.workspace, this.config.profile, p.model); }
  focusables(parent) {
    const result = [];
    const visit = widget => { if (widget.hidden || widget.detached) return; if (widget.options.keys || widget.type === 'button') result.push(widget); for (const child of widget.children || []) visit(child); };
    for (const child of parent.children) visit(child); return result;
  }
  renderStatus() {
    const p = this.config.profiles[this.config.profile];
    this.status.setContent(`${this.busy ? orange(this.progress || this.notice || 'Working…') : esc(this.notice || 'Ready')}  ${muted(`${p.label} / ${p.model || 'choose a model in Settings'} · ${this.config.permissions} · Context ≈ ${estimate(this.chat.messages.slice(this.chat.compacted))} / ${this.config.contextWindow}`)}`);
    this.tabs.forEach((widget, i) => { widget.style.bg = (this.helpModal ? i === 4 : this.tab === i) ? C.accent : C.surface; widget.style.fg = (this.helpModal ? i === 4 : this.tab === i) ? C.bg : C.muted; });
    this.screen.render();
  }
  resetBody() { for (const child of [...this.body.children]) child.destroy(); }
  actionBar(options = {}) { return box(this.body, { left: 0, right: 0, bottom: 0, height: 3, border: 'line', label: ' Actions ', ...options }); }
  selectTab(index) {
    if (index === 4) return this.showHelp();
    if (this.modal) return;
    this.tab = index; this.resetBody();
    if (index === 0) this.drawChat(); else if (index === 1) this.drawMCP(); else if (index === 2) this.drawAutomation(); else this.drawSettings();
    this.renderStatus();
  }
  setNotice(text) { this.notice = text; this.renderStatus(); }
  showHelp() {
    if (this.helpModal) { this.helpModal._close(); return; }
    if (this.modal) return;
    const content = 'Enter sends a message. Shift+Enter adds a line. Ctrl+S also sends.\nTab / Shift+Tab move focus. Ctrl+L focuses the composer.\nEscape stops a response or closes a dialog. Ctrl+Q exits.\nF1–F4 switch views. F5 opens help. ? toggles help outside text\nor in an empty composer. Ctrl+N creates a new chat.\n\n/new — start a new chat\n/fork — branch this conversation\n/continue — continue the current task\n/retry — branch before the last user request and retry it\n/compact — summarize earlier context; keep full history\n/attach PATH — add a workspace text file to the message\n/export — save the transcript as Markdown\n/events — show all agent activity\n/help — open this help\n\nHistory: select a saved chat to resume it. Find searches old chats.\nRename and Delete manage the selected conversation.\n\nWorkspace: ' + this.chat.workspace + '\nContext ≈ ' + estimate(this.chat.messages.slice(this.chat.compacted)) + ' / ' + this.config.contextWindow;
    const pending = this.message('Chat help', content);
    this.helpModal = this.modal; this.renderStatus();
    return pending.finally(() => { this.helpModal = null; this.renderStatus(); });
  }
  async reconnect() {
    this.setNotice('Connecting MCP servers…');
    this.config.workspace = fs.realpathSync(this.config.workspace);
    this.mcp.config = this.config;
    await this.mcp.connect();
    const failed = [...this.mcp.status].filter(([, status]) => status.state === 'error');
    this.setNotice(failed.length ? `MCP: ${failed.map(([name]) => name).join(', ')} failed — see MCP tab` : `${this.mcp.tools.size} MCP tools connected`);
  }
  transcript(chat = this.chat) {
    const lines = [], results = new Map(), waiting = new Map(), matchedResults = new Set();
    let speaker = '';
    this.toolRows = new Map();
    const append = content => lines.push(...content.split('\n'));
    // Pair each result with its preceding call. Message positions distinguish
    // calls even when a provider reuses IDs across different turns.
    chat.messages.forEach((message, index) => {
      for (const [callIndex, call] of (message.tool_calls || []).entries()) waiting.set(call.id, `${chat.id}:${index}:${callIndex}`);
      if (message.role === 'tool' && waiting.has(message.tool_call_id)) {
        results.set(waiting.get(message.tool_call_id), message.content); matchedResults.add(index); waiting.delete(message.tool_call_id);
      }
    });
    const toolRow = (key, name, args, result) => {
      const expanded = this.expandedTools.has(key);
      const state = result === undefined ? 'pending' : /^(Tool error:|Error:|Interrupted)/i.test(result) ? 'failed' : /^(Tool denied)/i.test(result) ? 'denied' : 'done';
      this.toolRows.set(lines.length, key);
      append(muted(`${expanded ? '▼' : '▶'} ${name} · ${state}`));
      if (!expanded) return;
      if (args !== undefined) {
        try { args = JSON.stringify(JSON.parse(args), null, 2); } catch {}
        append(muted(`  Arguments\n${String(args).split('\n').map(line => `  ${line}`).join('\n')}`));
      }
      append(muted(`  Result\n${String(result ?? 'Awaiting result…').split('\n').map(line => `  ${line}`).join('\n')}`));
    };
    for (const [index, message] of chat.messages.entries()) {
      if (message.role === 'user') {
        append(`\n${orange(capitalize(this.username))}\n${esc(message.content)}`); speaker = 'user';
      }
      else if (message.role === 'assistant') {
        if (message.content) append(`${speaker === 'Seth' ? '\n' : `\n${orange('Seth')}\n`}${this.markdown(message.content)}`);
        else if (message.tool_calls?.length && speaker !== 'Seth') append(`\n${orange('Seth')}`);
        if (message.content || message.tool_calls?.length) speaker = 'Seth';
        for (const [callIndex, call] of (message.tool_calls || []).entries()) {
          const key = `${chat.id}:${index}:${callIndex}`, tool = this.mcp?.tools.get(call.function.name);
          toolRow(key, tool ? `${tool.serverName} / ${tool.name}` : call.function.name, call.function.arguments, results.get(key));
        }
      } else if (message.role === 'tool' && !matchedResults.has(index)) toolRow(`${chat.id}:${index}:result`, 'Tool result', undefined, message.content);
    }
    if (chat.summary) append(`\n${orange('CONTEXT SUMMARY')}\n${esc(chat.summary)}`);
    if (this.partial) append(`${speaker === 'Seth' ? '\n' : `\n${orange('Seth')}\n`}${this.markdown(this.partial)}`);
    return lines.join('\n') || `${orange('Seth · your local assistant')}\n\nChoose a provider and model in Settings, then send a message.\n\nFiles and shell commands use the workspace chosen in Settings.\nWeb search and file tools are enabled by default.\n\nEnter sends. Shift+Enter adds a line. Escape stops.\nUse Tab to move between panes and buttons.\n\nF5 or the Help tab shows commands and shortcuts.`;
  }
  toggleToolAt(data) {
    const log = this.log, pos = log?.lpos;
    if (!pos || this.modal || (data.button && data.button !== 'left')) return;
    // Ignore borders, padding and the scrollbar. Convert screen coordinates to
    // wrapped content rows before looking up the original transcript line.
    if (data.x < pos.xi + log.ileft || data.x >= pos.xl - log.iright - 1 || data.y < pos.yi + log.itop || data.y >= pos.yl - log.ibottom) return;
    const offset = data.y - pos.yi - log.itop, row = offset + pos.base;
    const line = log._clines.rtof[row], key = this.toolRows.get(line);
    if (!key) return;
    if (this.expandedTools.has(key)) this.expandedTools.delete(key); else this.expandedTools.add(key);
    log.setContent(this.transcript());
    const newLine = [...this.toolRows].find(([, value]) => value === key)?.[0];
    const newRow = log._clines.ftor[newLine]?.[0];
    if (newRow !== undefined) log.setScroll(Math.max(0, newRow - offset));
    this.screen.render();
  }
  markdown(text) {
    let code = false;
    return clean(text).split('\n').map(line => {
      if (line.startsWith('```')) { code = !code; return muted(line); }
      if (code) return `  ${esc(line)}`;
      if (/^#{1,6} /.test(line)) return orange(line.replace(/^#+ /, ''));
      return esc(line).replace(/\*\*([^*]+)\*\*/g, '{bold}$1{/bold}');
    }).join('\n');
  }
  drawChat() {
    this.resetBody();
    const side = box(this.body, { left: 0, top: 0, bottom: 0, width: '26%', border: 'line', label: ' History ' });
    button(side, '+ New', 1, () => { if (!this.busy) { this.chat = this.fresh(); this.selectTab(0); } }, { top: 0 });
    button(side, 'Find', 13, () => this.findHistory(), { top: 0, width: 8 });
    const chats = this.store.chats().filter(chat => !this.historyQuery || `${chat.title} ${JSON.stringify(chat.messages)}`.toLowerCase().includes(this.historyQuery.toLowerCase()));
    this.history = blessed.list({ parent: side, top: 2, bottom: 3, width: '100%-2', left: 0, mouse: true, keys: true, vi: true, tags: false, scrollbar: { ch: ' ', style: { bg: C.surface } }, style: { fg: C.muted, bg: C.bg, selected: { fg: C.bg, bg: C.accent } }, items: chats.map(chat => `${chat.id === this.chat.id ? '› ' : '  '}${clean(chat.title)}\n  ${new Date(chat.updated).toLocaleDateString()}`) });
    const activeChat = chats.findIndex(chat => chat.id === this.chat.id);
    if (activeChat >= 0) this.history.select(activeChat);
    else { this.history.selected = -1; this.history.value = ''; }
    const openChat = index => {
      if (!chats[index]) return;
      if (this.busy) return this.setNotice('Stop the current response before switching chats.');
      this.loadChat(chats[index].id).catch(error => this.setNotice(error.message));
    };
    this.history.on('select', (_, index) => openChat(index));
    // Blessed only emits `select` on a second click; its item widgets always
    // emit `click`, so open the newly selected history row immediately.
    this.history.items.forEach(item => item.on('click', () => openChat(this.history.selected)));
    button(side, 'Rename', 1, () => this.renameChat(), { bottom: 1 });
    button(side, 'Delete', 13, () => this.deleteChat(), { bottom: 1, width: 10 });
    this.log = box(this.body, { left: '26%', top: 0, right: 0, bottom: 10, border: 'line', label: ` ${esc(this.chat.title)} `, padding: { left: 1, right: 1 }, scrollable: true, alwaysScroll: true, mouse: true, keys: true, vi: true, scrollbar: { ch: ' ', style: { bg: C.surface } }, content: this.transcript() });
    this.log.on('click', data => this.toggleToolAt(data));
    this.log.setScrollPerc(100);
    this.composer = new Editor(this.body, { left: '26%', bottom: 3, right: 0, height: 6, label: ' Message · Enter sends · Shift+Enter adds a line ', multiline: true, submitOnEnter: true, placeholder: 'What would you like to do?', value: this.draft || '', onSubmit: () => this.submit(), onChange: value => { this.draft = value; } });
    const actions = this.actionBar({ left: '26%' });
    button(actions, 'Send', 1, () => this.submit(), { top: 0 });
    button(actions, 'Stop', 13, () => this.controller?.abort(new Error('Stopped by user.')), { top: 0 });
    button(actions, 'Compact', 25, () => this.command('/compact'), { top: 0 });
    button(actions, 'Export', 40, () => this.exportChat(), { top: 0 });

    this.composer.widget.focus(); this.screen.render();
  }
  async loadChat(id) {
    const chat = this.store.loadChat(id); if (!chat) throw new Error('Chat no longer exists.');
    const workspace = fs.realpathSync(chat.workspace);
    this.chat = chat;
    if (this.config.workspace !== workspace) { this.config.workspace = workspace; await this.reconnect(); }
    if (PROFILES[chat.provider]) { this.config.profile = chat.provider; this.config.profiles[chat.provider].model = chat.model; }
    this.partial = ''; this.draft = ''; this.selectTab(0);
  }
  async findHistory() { const values = await this.form('Find chats', [{ key: 'query', label: 'Search titles and message text', value: this.historyQuery || '' }]); if (values) { this.historyQuery = values.query; this.selectTab(0); } }
  async renameChat() {
    if (this.busy) return;
    const values = await this.form('Rename chat', [{ key: 'title', label: 'Title', value: this.chat.title }]);
    if (values?.title.trim()) { this.chat.title = values.title.trim(); this.store.saveChat(this.chat); this.selectTab(0); }
  }
  async deleteChat() {
    if (this.busy || !this.store.chats().some(chat => chat.id === this.chat.id)) return;
    if (await this.confirm(`Delete “${this.chat.title}” and its saved transcript?`)) { this.store.deleteChat(this.chat.id); this.chat = this.fresh(); this.selectTab(0); }
  }
  async exportChat() {
    const values = await this.form('Export transcript', [{ key: 'path', label: 'Markdown file', value: path.join(this.chat.workspace, `chat-${this.chat.id.slice(0, 8)}.md`) }]);
    if (!values) return;
    try {
      const file = path.resolve(values.path); if (fs.existsSync(file) && !await this.confirm(`Overwrite ${file}?`)) return;
      const content = `# ${this.chat.title}\n\nWorkspace: ${this.chat.workspace}\n\n` + this.chat.messages.map(message => `## ${message.role}\n\n${message.content || ''}${message.tool_calls ? `\n\n${JSON.stringify(message.tool_calls, null, 2)}` : ''}`).join('\n\n');
      fs.writeFileSync(file, content, { mode: 0o600 }); this.setNotice(`Exported ${file}`);
    } catch (error) { this.setNotice(error.message); }
  }
  makeAgent() {
    return new Agent(this.store, this.config, this.mcp, {
      approve: (tool, args, signal) => this.approveTool(tool, args, signal),
      nativeTools, nativeCall: (name, args) => this.scheduler.native(name, args, this.config, this.mcp),
      onEvent: event => {
        if (event.type === 'delta') this.partial += event.text;
        else if (event.type === 'message') this.partial = '';
        else if (event.type === 'status') this.progress = event.text;
        else if (event.type === 'notice') this.notice = event.text;
        else if (event.type === 'tool') this.progress = `Using ${event.text}`;
        if (this.tab === 0 && this.log && !this.modal) { const atBottom = this.log.getScrollPerc() >= 95; this.log.setContent(this.transcript()); if (atBottom) this.log.setScrollPerc(100); }
        this.renderStatus();
      },
    });
  }
  async approveTool(tool, args, signal) {
    let detail = JSON.stringify(args, null, 2);
    if (this.config.mcpServers[tool.serverName]?.builtin === 'filesystem') {
      if (tool.name === 'edit_file') detail = `File: ${args.path}\n\nReplace:\n${args.old_text}\n\nWith:\n${args.new_text}`;
      if (tool.name === 'write_file') {
        const reader = [...this.mcp.tools.values()].find(item => item.serverName === tool.serverName && item.name === 'read_file');
        const current = reader ? await this.mcp.call(reader.publicName, { path: args.path, lines: 200 }, signal) : 'Read tool disabled.';
        detail = `File: ${args.path}\n\nCurrent file (first 200 lines):\n${current}\n\nProposed content:\n${String(args.content).slice(0, 16000)}`;
      }
    }
    return this.confirm(`Allow ${tool.serverName} / ${tool.name}?\n\n${detail}${tool.name === 'shell' ? '\n\nShell commands have your full OS permissions.' : ''}`, signal);
  }
  async submit() {
    if (this.busy || this.modal) return;
    const prompt = this.composer?.value.trim(); if (!prompt) return;
    this.draft = ''; this.composer.set('');
    if (prompt.startsWith('/')) return this.command(prompt);
    await this.run(prompt);
  }
  async run(prompt, resume = false) {
    if (this.busy) return;
    this.busy = true; this.controller = new AbortController(); this.partial = ''; this.notice = ''; this.progress = 'Thinking…'; this.renderStatus();
    try { await this.ready; this.notice = ''; this.activeRun = this.makeAgent().run(this.chat, prompt, { signal: this.controller.signal, resume }); await this.activeRun; }
    catch (error) { this.notice = this.controller.signal.aborted ? 'Stopped' : error.message; }
    finally { this.busy = false; this.partial = ''; this.progress = ''; if (this.tab === 0) this.selectTab(0); this.renderStatus(); }
  }
  async command(value) {
    if (this.busy) return;
    const [name, ...parts] = value.split(' '), argument = parts.join(' ').trim();
    try {
      if (name === '/new') { this.chat = this.fresh(); this.selectTab(0); }
      else if (name === '/fork') {
        const original = this.chat; this.chat = { ...structuredClone(original), id: this.fresh().id, title: `${original.title} (branch)` };
        this.store.event(this.chat, `Branched from ${original.id}.`); this.selectTab(0);
      }
      else if (name === '/help') await this.showHelp();
      else if (name === '/export') await this.exportChat();
      else if (name === '/events') await this.message('Activity', this.chat.events.map(event => `${event.at}  ${event.text}`).join('\n') || 'No activity yet.');
      else if (name === '/continue') await this.run('Continue the pending work. Inspect previous tool results before repeating actions.');
      else if (name === '/retry') {
        let index = this.chat.messages.length - 1; while (index >= 0 && this.chat.messages[index].role !== 'user') index--;
        if (index < 0) throw new Error('No request to retry.');
        const prompt = this.chat.messages[index].content, original = this.chat;
        this.chat = this.fresh(); this.chat.title = `${original.title} (retry)`; this.chat.messages = structuredClone(original.messages.slice(0, index));
        if (original.compacted <= index) { this.chat.summary = original.summary; this.chat.compacted = original.compacted; }
        this.store.event(this.chat, `Branched from ${original.id}; previous tool side effects are preserved.`); await this.run(prompt);
      } else if (name === '/compact') {
        this.busy = true; this.controller = new AbortController(); this.notice = ''; this.progress = 'Compacting earlier context…'; this.renderStatus();
        try { await this.ready; const unlock = this.store.lock(`chat-${this.chat.id}`); try { await this.makeAgent().compact(this.chat, this.controller.signal, true); } finally { unlock(); } }
        finally { this.busy = false; this.progress = ''; this.selectTab(0); }
      } else if (name === '/attach') {
        await this.ready;
        const tool = [...this.mcp.tools.values()].find(tool => this.config.mcpServers[tool.serverName]?.builtin === 'filesystem' && tool.name === 'read_file');
        if (!tool) throw new Error('Enable the filesystem MCP server first.');
        const text = await this.mcp.call(tool.publicName, { path: argument, start: 1, lines: 1000 });
        if (text.startsWith('Tool error:')) throw new Error(text);
        this.composer.set(`Please use this attached file (${argument}):\n\n${text}\n\n`); this.draft = this.composer.value;
      } else throw new Error('Unknown command. Use /help.');
    } catch (error) { this.setNotice(error.message); }
  }
  drawMCP() {
    if (this.tab !== 1) return;
    this.resetBody();
    const entries = Object.entries(this.config.mcpServers);
    const list = blessed.list({ parent: this.body, top: 0, left: 0, width: '40%', bottom: 3, border: 'line', label: ' Servers ', padding: { left: 1 }, mouse: true, keys: true, vi: true, style: { ...style, selected: { bg: C.accent, fg: C.bg } }, tags: false, items: entries.map(([name, server]) => { const status = this.mcp?.status.get(name); return `${name}  ${server.enabled === false ? 'disabled' : status?.state || 'pending'}  (${status?.tools || 0} tools)`; }) });
    const details = box(this.body, { top: 0, left: '40%', right: 0, bottom: 3, border: 'line', label: ' Server details ', padding: { left: 1, right: 1 }, scrollable: true, mouse: true, keys: true });
    const selected = () => entries[list.selected];
    const inspect = () => {
      const entry = selected(); if (!entry) return; const [name, server] = entry, status = this.mcp?.status.get(name);
      const tools = [...(this.mcp?.tools.values() || [])].filter(tool => tool.serverName === name);
      const divider = muted('─'.repeat(Math.max(12, Number(details.width) - 4)));
      details.setContent(`${orange(name)}  ${muted(status?.state || 'pending')}\n${esc(status?.era || server.transport || (server.url ? 'HTTP' : 'stdio'))}\n${esc(server.builtin ? `Bundled ${server.builtin}` : server.url || [server.command, ...(server.args || [])].join(' '))}\n${divider}\n${tools.map(tool => `${orange(tool.name)}  ${muted(tool.safe ? 'read only' : 'asks for approval')}\n${esc(tool.definition.function.description.replace(/^\[[^\]]+\] /, ''))}`).join(`\n${divider}\n`)}${status?.details ? `\n${divider}\n${orange('Connection log')}\n${esc(status.details)}` : ''}`); this.screen.render();
    };
    list.on('select item', inspect); inspect();
    const actions = this.actionBar();
    button(actions, 'Add', 1, () => this.addServer(), { top: 0 });
    button(actions, 'Import', 10, () => this.importServers(), { top: 0 });
    button(actions, 'Edit', 22, async () => { const entry = selected(); if (entry) await this.addServer(entry); }, { top: 0 });
    button(actions, 'Toggle', 32, async () => { const entry = selected(); if (!entry || this.busy) return; entry[1].enabled = entry[1].enabled === false; await this.saveAndConnect(); }, { top: 0 });
    button(actions, 'Tools', 44, () => { const entry = selected(); if (entry) this.manageTools(entry[0]); }, { top: 0 });
    button(actions, 'Remove', 55, async () => { const entry = selected(); if (!entry || this.busy) return; if (await this.confirm(`Remove ${entry[0]}?`)) { delete this.config.mcpServers[entry[0]]; await this.saveAndConnect(); } }, { top: 0 });
    button(actions, 'Reconnect', 68, () => { if (!this.busy) this.saveAndConnect(); }, { top: 0 });
    list.focus(); this.screen.render();
  }
  async saveAndConnect() {
    try { this.store.saveConfig(this.config); this.ready = this.reconnect(); await this.ready; this.selectTab(this.tab); }
    catch (error) { this.setNotice(error.message); }
  }
  async addServer(entry) {
    if (this.busy) return;
    if (!entry) {
      const type = await this.choose('Add MCP server', ['Local command (stdio)', 'Remote MCP endpoint', 'Paste server JSON']);
      if (type === null) return;
      if (type < 2) {
        const fields = [{ key: 'name', label: 'Server name', value: '' }];
        if (type === 0) fields.push({ key: 'command', label: 'Program to launch', value: 'npx' }, { key: 'args', label: 'Arguments · one per line', value: '-y\n', multiline: true, height: 5 });
        else fields.push({ key: 'url', label: 'MCP endpoint URL', value: 'http://127.0.0.1:3000/mcp' }, { key: 'token', label: 'Bearer token environment variable (optional)', value: '' });
        const values = await this.form('Add MCP server', fields); if (!values) return;
        try {
          if (!/^[\w-]{1,48}$/.test(values.name) || this.config.mcpServers[values.name]) throw new Error('Choose a unique server name using letters, numbers, underscores or hyphens.');
          const server = type === 0 ? { command: values.command.trim(), args: values.args.split('\n').map(line => line.trim()).filter(Boolean), enabled: true } : { url: values.url.trim(), enabled: true, ...(values.token.trim() ? { headers: { Authorization: `Bearer \${${values.token.trim()}}` } } : {}) };
          validateServer(server);
          if (!await this.confirm(`Connect ${values.name}?\n\n${server.url || [server.command, ...server.args].join(' ')}${type === 0 ? '\n\nThis process runs with your user permissions.' : ''}`)) return;
          this.config.mcpServers[values.name] = server; await this.saveAndConnect();
        } catch (error) { this.setNotice(error.message); }
        return;
      }
    }
    const values = await this.form(entry ? 'Edit MCP server' : 'Add MCP server', [
      { key: 'name', label: 'Name', value: entry?.[0] || '' },
      { key: 'json', label: 'Server JSON · command/args/env or url/headers', multiline: true, height: 10, value: JSON.stringify(entry?.[1] || { command: 'npx', args: ['-y', '@modelcontextprotocol/server-filesystem', this.config.workspace], enabled: true }, null, 2) },
    ]);
    if (!values) return;
    try {
      if (!/^[\w-]{1,48}$/.test(values.name)) throw new Error('Use a name with letters, numbers, underscores or hyphens.');
      if (values.name !== entry?.[0] && this.config.mcpServers[values.name]) throw new Error('A server with that name already exists.');
      const server = validateServer(JSON.parse(values.json));
      if (!server.builtin && !await this.confirm(`Enable MCP server ${values.name}?\n\n${server.url || [server.command, ...(server.args || [])].join(' ')}\n\nLocal server processes run with your user permissions.`)) return;
      if (entry && entry[0] !== values.name) delete this.config.mcpServers[entry[0]];
      this.config.mcpServers[values.name] = server; await this.saveAndConnect();
    } catch (error) { this.setNotice(error.message); }
  }
  async importServers() {
    if (this.busy) return;
    const values = await this.form('Import MCP servers', [{ key: 'json', label: 'Paste JSON (or an absolute JSON file path)', multiline: true, height: 12, value: '{"mcpServers": {}}' }]); if (!values) return;
    try {
      const text = values.json.trim(), imported = JSON.parse(text.startsWith('/') ? fs.readFileSync(text, 'utf8') : text);
      const servers = imported.mcpServers || imported;
      if (!servers || typeof servers !== 'object' || Array.isArray(servers)) throw new Error('Expected a server object.');
      for (const [name, server] of Object.entries(servers)) { if (!/^[\w-]{1,48}$/.test(name)) throw new Error(`Invalid server name: ${name}`); validateServer(server); }
      if (!Object.keys(servers).length) return this.setNotice('No servers to import.');
      if (!await this.confirm(`Import ${Object.keys(servers).join(', ')}? Existing names will be replaced. Enabled local commands will be launched.`)) return;
      Object.assign(this.config.mcpServers, servers); await this.saveAndConnect();
    } catch (error) { this.setNotice(error.message); }
  }
  async manageTools(name) {
    if (this.busy) return;
    const server = this.config.mcpServers[name];
    const tools = [...this.mcp.tools.values()].filter(tool => tool.serverName === name && !tool.name.startsWith('$'));
    const choice = await this.choose('Tool controls', [...tools.map(tool => `Disable ${tool.name}`), ...(server.disabledTools || []).map(tool => `Enable ${tool}`)]);
    if (choice === null) return;
    if (choice < tools.length) server.disabledTools = [...new Set([...(server.disabledTools || []), tools[choice].name])];
    else server.disabledTools.splice(choice - tools.length, 1);
    await this.saveAndConnect();
  }
  drawAutomation() {
    this.resetBody();
    const tasks = this.store.tasks();
    const list = blessed.list({ parent: this.body, top: 0, left: 0, width: '36%', bottom: 3, border: 'line', label: ' Tasks ', padding: { left: 1 }, mouse: true, keys: true, vi: true, style: { ...style, selected: { bg: C.accent, fg: C.bg } }, tags: false, items: tasks.map(task => `${task.enabled ? '●' : '○'} ${clean(task.name)} · ${task.lastStatus}`) });
    const details = box(this.body, { top: 0, left: '36%', right: 0, bottom: 3, border: 'line', label: ' Task details ', padding: { left: 1, right: 1 }, scrollable: true, mouse: true, keys: true });
    const selected = () => tasks[list.selected];
    const inspect = () => { const task = selected(); details.setContent(task ? `${orange(task.name)}\n${esc(task.prompt)}\n\n${muted('Schedule')} ${esc(task.cron)} (${esc(task.timezone)})\n${muted('Next')} ${esc(new Date(task.nextRun).toLocaleString())}\n${muted('Last')} ${esc(task.lastRun || 'Never')}\n${muted('Result')} ${esc(task.lastStatus)}\n${muted('Workspace')} ${esc(task.workspace)}\n${muted('Model')} ${esc(task.profile)} / ${esc(task.model)}\n${muted('Allowed')} read tools${task.allowedTools.length ? ` + ${esc(task.allowedTools.join(', '))}` : ''}\n\n${esc(task.lastError || '')}` : `${orange('No scheduled tasks')}\n\nAdd a task, choose its cron schedule, then enable the timer. Results appear in History.`); this.screen.render(); };
    list.on('select item', inspect); inspect();
    const actions = this.actionBar();
    button(actions, 'Add', 1, () => this.editTask(), { top: 0 });
    button(actions, 'Edit', 10, () => { const task = selected(); if (task) this.editTask(task); }, { top: 0 });
    button(actions, 'Pause', 20, () => { const task = selected(); if (task) { this.scheduler.update(task.id, { enabled: !task.enabled }); this.selectTab(2); } }, { top: 0 });
    button(actions, 'Run now', 31, async () => {
      const task = selected(); if (!task || this.busy) return;
      if (!await this.confirm(`Run ${task.name} now with its saved tool permissions?`)) return;
      this.busy = true; this.controller = new AbortController(); this.setNotice(`Running ${task.name}…`);
      try { this.activeRun = this.scheduler.run(task.id, { signal: this.controller.signal }); await this.activeRun; this.notice = `${task.name} completed`; }
      catch (error) { this.notice = error.message; }
      finally { this.busy = false; this.selectTab(this.tab); }
    }, { top: 0 });
    button(actions, 'Result', 44, () => { const task = selected(); if (task?.lastChat && !this.busy) this.loadChat(task.lastChat).catch(error => this.setNotice(error.message)); }, { top: 0 });
    button(actions, 'Delete', 56, async () => { const task = selected(); if (task && await this.confirm(`Delete ${task.name}?`)) { this.scheduler.remove(task.id); this.selectTab(2); } }, { top: 0 });
    button(actions, 'Timer', 68, () => this.manageTimer(), { top: 0 });
    list.focus(); this.screen.render();
  }
  async editTask(task) {
    if (this.busy) return;
    const values = await this.form(task ? 'Edit scheduled task' : 'New scheduled task', [
      { key: 'name', label: 'Task name', value: task?.name || '' },
      { key: 'cron', label: 'Cron: minute hour day month weekday · 0 9 * * 1-5 = weekdays 09:00', value: task?.cron || '0 9 * * *' },
      { key: 'timezone', label: 'Timezone', value: task?.timezone || Intl.DateTimeFormat().resolvedOptions().timeZone },
      { key: 'workspace', label: 'Workspace', value: task?.workspace || this.config.workspace },
      { key: 'prompt', label: 'Instructions', multiline: true, height: 5, value: task?.prompt || '' },
    ]); if (!values) return;
    try {
      const allowedTools = await this.chooseAllowedTools(task?.allowedTools || []);
      if (allowedTools === null) return;
      if (allowedTools.length && !await this.confirm(`Allow these tools without prompts for this scheduled task?\n\n${allowedTools.join('\n')}`)) return;
      const config = structuredClone(this.config); config.workspace = fs.realpathSync(values.workspace);
      this.scheduler.save({ name: values.name, prompt: values.prompt, cron: values.cron, timezone: values.timezone, allowedTools, enabled: task?.enabled ?? true }, config, this.mcp, task?.id);
      this.selectTab(2); this.setNotice('Task saved. Enable Timer for background execution.');
    } catch (error) { this.setNotice(error.message); }
  }
  async chooseAllowedTools(existing) {
    const tools = [...this.mcp.tools.values()].filter(tool => !tool.safe);
    const selected = new Set(existing.filter(name => this.mcp.tools.has(name)));
    while (true) {
      const choice = await this.choose('Background permissions · read tools always allowed', ['Save tool permissions', 'Clear extra permissions (read only)', ...tools.map(tool => `${selected.has(tool.publicName) ? '[x]' : '[ ]'} ${tool.serverName} / ${tool.name}`)]);
      if (choice === null) return null;
      if (choice === 0) return [...selected];
      if (choice === 1) { selected.clear(); continue; }
      const name = tools[choice - 2].publicName; if (selected.has(name)) selected.delete(name); else selected.add(name);
    }
  }
  async manageTimer() {
    if (this.busy) return;
    const status = await this.scheduler.timerStatus();
    const choice = await this.choose(`Background timer: ${status}`, ['Enable background scheduler', 'Disable background scheduler']); if (choice === null) return;
    try { if (choice === 0) await this.scheduler.enable(); else await this.scheduler.disable(); this.setNotice(await this.scheduler.timerStatus()); }
    catch (error) { this.setNotice(`Timer: ${error.message}`); }
  }
  drawSettings() {
    this.resetBody(); const p = this.config.profiles[this.config.profile];
    const entries = [
      ['profile', 'Provider', p.label, 'Choose a preset. Each preset remembers its own endpoint, model and credentials.'],
      ['endpoint', 'Endpoint', p.endpoint, 'OpenAI-compatible base URL, usually ending in /v1.'],
      ['model', 'Model', p.model || 'Not selected', 'Discover models from the endpoint or enter a model identifier. The model must support tool calls.'],
      ['apiKey', 'API key', p.apiKey ? '••••••••' : 'Not set', 'Optional. Stored in the private settings file. Prefer an environment variable.'],
      ['apiKeyEnv', 'API key environment variable', p.apiKeyEnv || 'Not set', 'Variable name, e.g. LOCAL_AGENT_API_KEY. Takes precedence over the stored key.'],
      ['workspace', 'Workspace', this.config.workspace, 'Filesystem tools are scoped to this directory. Shell commands have full user permissions.'],
      ['permissions', 'Tool permissions', this.config.permissions, 'ask: approve changes and external tools. read-only: bundled read tools only. auto: all connected tools without prompts.'],
      ['contextWindow', 'Context window (tokens)', this.config.contextWindow, 'Set this to the actual context size configured in your local model server. Auto compaction reserves room for replies and tool definitions.'],
      ['maxTokens', 'Output limit (tokens)', this.config.maxTokens, 'Maximum generated tokens per model response. Must be less than half the context window.'],
      ['maxSteps', 'Tool step limit', this.config.maxSteps, 'Maximum model/tool rounds per request. /continue resumes work after the limit.'],
      ['timeout', 'Request timeout (seconds)', this.config.timeout, 'Timeout for model generation and MCP calls.'],
      ['systemPrompt', 'System instructions', this.config.systemPrompt.slice(0, 60), 'Persistent instructions for every chat.'],
    ];
    const list = blessed.list({ parent: this.body, top: 0, bottom: 3, left: 0, width: '62%', border: 'line', label: ' Preferences ', padding: { left: 1 }, mouse: true, keys: true, vi: true, style: { ...style, selected: { fg: C.bg, bg: C.accent } }, tags: false, items: entries.map(([, label, value]) => `${label}: ${clean(value)}`) });
    const detail = box(this.body, { top: 0, bottom: 3, left: '62%', right: 0, border: 'line', label: ' About ', padding: { left: 1, right: 1 }, content: esc(entries[0][3]) });
    list.on('select item', () => { detail.setContent(esc(entries[list.selected]?.[3] || '')); this.screen.render(); });
    list.on('select', (_, index) => this.editSetting(entries[index]));
    const actions = this.actionBar();
    button(actions, 'Edit selected', 1, () => this.editSetting(entries[list.selected]), { top: 0 });
    button(actions, 'Test connection', 22, () => this.discoverModels(true), { top: 0 });
    list.focus(); this.screen.render();
  }
  async editSetting(entry) {
    if (!entry || this.busy) return;
    const [key, label] = entry, draft = structuredClone(this.config), p = draft.profiles[draft.profile];
    try {
      if (key === 'profile') { const names = Object.keys(PROFILES); const index = await this.choose('Provider preset', names.map(name => PROFILES[name].label)); if (index === null) return; draft.profile = names[index]; }
      else if (key === 'model') { const model = await this.discoverModels(); if (model === undefined) return; p.model = model; }
      else if (key === 'permissions') {
        const values = ['ask', 'read-only', 'auto'];
        const index = await this.choose('Tool permissions', ['Ask before changes and external tools', 'Read only (bundled tools)', 'Auto (all tools, remembered)'], { selected: values.indexOf(draft.permissions) });
        if (index === null || values[index] === draft.permissions) return;
        if (index === 2 && !await this.confirm('Enable Auto for all connected tools, including shell commands?\n\nThis setting is saved for all interactive chats and future sessions, until you change it in Settings.', null, 'Automatic tool permissions', false, { allowLabel: 'Enable Auto' })) return;
        draft.permissions = values[index];
      }
      else {
        const value = ['endpoint', 'apiKey', 'apiKeyEnv'].includes(key) ? p[key] : draft[key];
        const values = await this.form(label, [{ key: 'value', label, value: String(value), multiline: key === 'systemPrompt', height: key === 'systemPrompt' ? 10 : 3, secret: key === 'apiKey' }]); if (!values) return;
        if (['endpoint', 'apiKey', 'apiKeyEnv'].includes(key)) p[key] = values.value.trim();
        else if (['contextWindow', 'maxTokens', 'maxSteps', 'timeout'].includes(key)) draft[key] = Number(values.value);
        else if (key === 'workspace') draft.workspace = fs.realpathSync(values.value);
        else draft[key] = values.value;
      }
      this.store.saveConfig(draft); const changedWorkspace = draft.workspace !== this.config.workspace;
      this.config = draft;
      if (!this.chat.messages.length || changedWorkspace) this.chat = this.fresh();
      else { this.chat.provider = draft.profile; this.chat.model = draft.profiles[draft.profile].model; this.store.saveChat(this.chat); }
      await this.saveAndConnect();
    } catch (error) { this.setNotice(error.message); }
  }
  async discoverModels(test = false) {
    try {
      this.setNotice('Querying provider models…'); const models = await new Provider(this.config).models();
      if (test) { await this.message('Provider connection', `Connected to ${this.config.profiles[this.config.profile].endpoint}\n\n${models.length} models:\n${models.join('\n') || '(none loaded)'}`); return; }
      const choice = await this.choose('Select model', [...models, 'Enter a model identifier…']); if (choice === null) return;
      if (choice < models.length) return models[choice];
    } catch (error) { this.setNotice(error.message); if (test) return; }
    const values = await this.form('Model identifier', [{ key: 'model', label: 'Model (exact identifier from your provider)', value: this.config.profiles[this.config.profile].model }]);
    return values?.model.trim();
  }
  form(title, fields) {
    if (this.modal) return Promise.resolve(null);
    return new Promise(resolve => {
      const prior = this.screen.focused;
      const height = Math.min(this.screen.height - 2, fields.reduce((sum, field) => sum + (field.multiline ? field.height || 6 : 3) + 1, 0) + 5);
      const modal = box(this.screen, { left: 'center', top: 'center', width: '90%', height, border: 'line', label: ` ${esc(title)} `, style: { ...style, bg: C.surface } }); this.modal = modal;
      const body = box(modal, { top: 0, bottom: 3, left: 1, right: 1, scrollable: true, style: { ...style, bg: C.surface } });
      const editors = []; let top = 0;
      for (const field of fields) {
        box(body, { top, height: 1, left: 0, right: 0, content: muted(field.label), style: { ...style, bg: C.surface } }); top++;
        const h = field.multiline ? field.height || 6 : 3;
        const editor = new Editor(body, { top, height: h, left: 0, right: 0, value: field.value || '', multiline: field.multiline, secret: field.secret, onSubmit: () => { if (fields.length === 1) finish(true); else this.screen.focusNext(); } });
        editor.widget.on('focus', () => { body.setScroll(Math.max(0, Number(editor.widget.top) + h - Number(body.height) + 1)); this.screen.render(); });
        editors.push([field.key, editor]); top += h;
      }
      const finish = saved => { if (this.modal !== modal) return; this.modal = null; const values = saved ? Object.fromEntries(editors.map(([key, editor]) => [key, editor.value])) : null; modal.destroy(); prior?.focus(); this.screen.render(); resolve(values); };
      modal.key('escape', () => finish(false));
      const onSave = () => finish(true), onEsc = () => finish(false);
      this.screen.key('C-s', onSave); this.screen.key('escape', onEsc);
      modal.on('destroy', () => { this.screen.unkey('C-s', onSave); this.screen.unkey('escape', onEsc); });
      button(modal, 'Save', 1, onSave, { bottom: 1 }); button(modal, 'Cancel', 13, onEsc, { bottom: 1 });
      box(modal, { bottom: 0, left: 1, height: 1, width: '100%-4', content: muted('Tab: next field · Ctrl+S: save · Esc: cancel'), style: { ...style, bg: C.surface } });
      editors[0]?.[1].widget.focus(); this.screen.render();
    });
  }
  choose(title, values, { selected = 0 } = {}) {
    if (this.modal || !values.length) return Promise.resolve(null);
    return new Promise(resolve => {
      const prior = this.screen.focused;
      const modal = box(this.screen, { left: 'center', top: 'center', width: '85%', height: Math.min(this.screen.height - 2, values.length + 4), border: 'line', label: ` ${esc(title)} ` }); this.modal = modal;
      const list = blessed.list({ parent: modal, top: 0, bottom: 1, left: 1, right: 1, mouse: true, keys: true, vi: true, items: values.map(clean), style: { ...style, selected: { fg: C.bg, bg: C.accent } } });
      list.select(selected);
      const finish = index => { if (this.modal !== modal) return; this.modal = null; modal.destroy(); prior?.focus(); this.screen.render(); resolve(index); };
      list.on('select', (_, index) => finish(index)); const cancel = () => finish(null); this.screen.key('escape', cancel); modal.on('destroy', () => this.screen.unkey('escape', cancel));
      box(modal, { bottom: 0, left: 1, height: 1, width: '100%-4', content: muted('↑/↓ choose · Enter select · Esc cancel') }); list.focus(); this.screen.render();
    });
  }
  message(title, content) { return this.confirm(content, null, title, true); }
  confirm(content, signal, title = 'Approval', info = false, { allowLabel = 'Allow once' } = {}) {
    if (signal?.aborted || this.modal) return Promise.resolve(false);
    return new Promise(resolve => {
      const prior = this.screen.focused;
      const modal = box(this.screen, { left: 'center', top: 'center', width: '88%', height: '85%', border: 'line', label: ` ${esc(title)} ` }); this.modal = modal;
      const detail = box(modal, { top: 0, bottom: 3, left: 1, right: 1, scrollable: true, mouse: true, keys: true, content: esc(content) });
      const finish = result => { if (this.modal !== modal) return; this.modal = null; signal?.removeEventListener('abort', cancel); modal.destroy(); prior?.focus(); this.screen.render(); resolve(result); };
      const cancel = () => finish(false); this.screen.key('escape', cancel); modal.on('destroy', () => this.screen.unkey('escape', cancel)); signal?.addEventListener('abort', cancel, { once: true });
      modal._close = cancel;
      const deny = button(modal, info ? 'Close' : 'Deny', 1, () => finish(false), { bottom: 1 });
      if (!info) button(modal, allowLabel, 13, () => finish(true), { bottom: 1 });
      box(modal, { bottom: 0, left: 1, height: 1, width: '100%-4', content: muted('Tab changes focus · Enter selects · Esc cancels · Scroll to inspect arguments') });
      deny.focus(); this.screen.render();
    });
  }
  async elicit(name, params) {
    if (params.mode === 'url') { await this.message('MCP sign-in required', `${name}: ${params.message}\n\nOpen this URL in your browser:\n${params.url}`); return { action: 'cancel' }; }
    const properties = params.requestedSchema?.properties || {}, fields = Object.entries(properties).map(([key, schema]) => ({ key, label: `${schema.title || key}${schema.description ? ` · ${schema.description}` : ''}`, value: String(schema.default ?? '') }));
    if (!fields.length) return { action: 'cancel' };
    const values = await this.form(`${name}: ${params.message}`, fields); if (!values) return { action: 'cancel' };
    const content = {};
    for (const [key, schema] of Object.entries(properties)) {
      const value = values[key]; if (!value && !params.requestedSchema.required?.includes(key)) continue;
      if (schema.type === 'boolean') { if (!['true', 'false'].includes(value)) return { action: 'decline' }; content[key] = value === 'true'; }
      else if (['number', 'integer'].includes(schema.type)) { content[key] = Number(value); if (!Number.isFinite(content[key])) return { action: 'decline' }; }
      else content[key] = value;
    }
    return { action: 'accept', content };
  }
  async quit() {
    if (this.quitting) return; this.quitting = true;
    this.controller?.abort(new Error('Agent closed.'));
    await this.activeRun?.catch(() => {});
    await this.ready?.catch(() => {});
    await this.mcp.close(); this.screen.destroy(); process.exit(0);
  }
}
module.exports = { TUI, Editor, clean };

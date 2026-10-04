'use strict';
const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const http = require('node:http');
const { PassThrough, Writable, Readable } = require('node:stream');
const { Store, repairMessages, validateConfig } = require('../store.cjs');
const { MCP, alias } = require('../mcp.cjs');
const { Agent, estimate } = require('../agent.cjs');
const { Provider } = require('../provider.cjs');
const { Scheduler, nextRun } = require('../automation.cjs');
const { resolveInside, shell } = require('../servers/filesystem.cjs');
const { parseSearch } = require('../servers/web.cjs');
let temp;
before(() => {
  temp = fs.mkdtempSync(path.join(os.tmpdir(), 'dotfiles-agent-test-'));
  for (const [variable, name] of [['XDG_CONFIG_HOME', 'config'], ['XDG_DATA_HOME', 'data'], ['XDG_STATE_HOME', 'state']]) process.env[variable] = path.join(temp, name);
});
after(() => fs.rmSync(temp, { recursive: true, force: true }));
async function server(handler) {
  const app = http.createServer(handler); await new Promise(resolve => app.listen(0, '127.0.0.1', resolve));
  return { endpoint: `http://127.0.0.1:${app.address().port}/v1`, close: () => new Promise(resolve => { app.close(resolve); app.closeAllConnections(); }) };
}
async function body(request) { const chunks = []; for await (const chunk of request) chunks.push(chunk); return JSON.parse(Buffer.concat(chunks).toString()); }
function config(store, endpoint) {
  const value = store.config(); value.workspace = temp;
  if (endpoint) value.profiles.lmstudio.endpoint = endpoint;
  value.profiles.lmstudio.model = 'test-model'; return value;
}
function find(mcp, name) { const tool = [...mcp.tools.values()].find(tool => tool.name === name); assert.ok(tool, name); return tool; }

test('private persistence, process locks and interrupted tool recovery', () => {
  const store = new Store(), value = config(store); store.saveConfig(value);
  assert.match(value.systemPrompt, /^You are Seth, a local Debian AI agent\./);
  const oldPrompt = value.systemPrompt.replace('You are Seth, a local Debian AI agent.', 'You are a local Debian AI agent.') + '\nKeep my custom workspace rule.';
  store.saveConfig({ ...value, systemPrompt: oldPrompt });
  const migrated = new Store().config();
  assert.match(migrated.systemPrompt, /^You are Seth, a local Debian AI agent\./);
  assert.match(migrated.systemPrompt, /Keep my custom workspace rule\.$/);
  assert.equal(JSON.parse(fs.readFileSync(store.configFile, 'utf8')).systemPrompt, migrated.systemPrompt);
  store.saveConfig({ ...value, systemPrompt: 'Custom instructions: review files carefully.' });
  assert.equal(new Store().config().systemPrompt, 'Custom instructions: review files carefully.');
  store.saveConfig(value);
  assert.equal(fs.statSync(store.configFile).mode & 0o777, 0o600);
  assert.throws(() => validateConfig({ ...value, maxTokens: value.contextWindow }), /Output tokens/);
  const release = store.lock('test'); assert.throws(() => store.lock('test'), /already running/); release();
  const chat = store.newChat(temp, 'lmstudio', 'test');
  chat.messages.push({ role: 'assistant', content: '', tool_calls: [{ id: 'call', function: { name: 'read_file', arguments: '{}' } }] });
  repairMessages(chat); repairMessages(chat); assert.equal(chat.messages.length, 2); assert.equal(chat.messages[1].tool_call_id, 'call');
  store.saveChat(chat); assert.equal(store.loadChat(chat.id).messages.length, 2);
});

test('workspace rejects traversal, outside and dangling symlinks', async () => {
  const workspace = path.join(temp, 'workspace'); fs.mkdirSync(workspace);
  fs.symlinkSync('/etc', path.join(workspace, 'outside')); fs.symlinkSync('/tmp/agent-nonexistent-target', path.join(workspace, 'dangling'));
  await assert.rejects(resolveInside(workspace, '../config'), /outside/);
  await assert.rejects(resolveInside(workspace, 'outside/passwd'), /outside/);
  await assert.rejects(resolveInside(workspace, 'outside/new-file', true), /outside/);
  await assert.rejects(resolveInside(workspace, 'dangling', true), /Dangling/);
  assert.equal(await resolveInside(workspace, 'new/folder/file', true), path.join(workspace, 'new/folder/file'));
});

test('real MCP v2 discovery, file checkpoints, restore and tool controls', async () => {
  const store = new Store(), value = config(store); const mcp = new MCP(value);
  try {
    await mcp.connect(); assert.equal(mcp.connections.size, 2);
    assert.equal(mcp.status.get('filesystem').era, 'MCP v2 / 2026'); assert.equal(mcp.status.get('web').era, 'MCP v2 / 2026');
    const write = find(mcp, 'write_file'), read = find(mcp, 'read_file'); assert.equal(write.safe, false); assert.equal(read.safe, true);
    assert.match(await mcp.call(write.publicName, { path: 'note.txt', content: 'first\n' }), /Checkpoint:/);
    const output = await mcp.call(write.publicName, { path: 'note.txt', content: 'second\n', expected_content: 'first\n' });
    await mcp.call(find(mcp, 'restore_checkpoint').publicName, { id: output.match(/Checkpoint: ([a-f0-9-]+)/)[1] });
    assert.match(await mcp.call(read.publicName, { path: 'note.txt' }), /first/);
    assert.match(await mcp.call(read.publicName, { path: '/etc/passwd' }), /outside/);
    value.mcpServers.filesystem.disabledTools = ['shell']; value.mcpServers.web.enabled = false;
    await mcp.connect(); assert.equal([...mcp.tools.values()].some(tool => tool.name === 'shell'), false); assert.equal(mcp.status.get('web').state, 'disabled');
    assert.ok(alias('a', 'b').length < 64); assert.notEqual(alias('a', 'b-c'), alias('a', 'b_c'));
  } finally { await mcp.close(); }
});

test('MCP v2 Streamable HTTP, resources and prompt bridge', async () => {
  const { McpServer, createMcpHandler } = require('@modelcontextprotocol/server'); const z = require('zod/v4');
  const handler = createMcpHandler(() => {
    const sdk = new McpServer({ name: 'http-fixture', version: '1' });
    sdk.registerTool('echo', { inputSchema: z.object({ text: z.string() }) }, async args => ({ content: [{ type: 'text', text: args.text }] }));
    sdk.registerResource('note', 'note://example', {}, async uri => ({ contents: [{ uri: uri.href, text: 'Resource works' }] }));
    sdk.registerPrompt('greet', { description: 'Greeting' }, async () => ({ messages: [{ role: 'user', content: { type: 'text', text: 'Hello' } }] }));
    return sdk;
  }, { legacy: 'reject' });
  const app = await server(async (request, response) => {
    try {
      const chunks = []; for await (const chunk of request) chunks.push(chunk);
      const controller = new AbortController(); response.on('close', () => controller.abort());
      const web = new Request(`http://127.0.0.1${request.url}`, { method: request.method, headers: request.headers, signal: controller.signal, ...(['GET', 'HEAD'].includes(request.method) ? {} : { body: Buffer.concat(chunks) }) });
      const result = await handler.fetch(web); response.writeHead(result.status, Object.fromEntries(result.headers)); response.flushHeaders();
      if (result.body) for await (const chunk of Readable.fromWeb(result.body)) response.write(chunk);
      response.end();
    } catch (error) { if (!response.headersSent) response.writeHead(500); response.end(error.message); }
  });
  const value = config(new Store()); value.mcpServers = { http: { url: app.endpoint, enabled: true } }; const mcp = new MCP(value);
  try {
    await mcp.connect(); assert.equal(mcp.status.get('http').state, 'connected', mcp.status.get('http').details);
    assert.equal(await mcp.call(find(mcp, 'echo').publicName, { text: 'HTTP works' }), 'HTTP works');
    assert.match(await mcp.call(find(mcp, '$read').publicName, { uri: 'note://example' }), /Resource works/);
    assert.match(await mcp.call(find(mcp, '$prompt').publicName, { name: 'greet' }), /Hello/);
  } finally { await mcp.close(); await handler.close?.(); await app.close(); }
});

test('provider handles chunked streaming, UTF-8, models and JSON fallback', async () => {
  const app = await server(async (request, response) => {
    if (request.method === 'GET') return response.end(JSON.stringify({ data: [{ id: 'test-model' }] }));
    const data = await body(request);
    if (!data.stream) return response.end(JSON.stringify({ choices: [{ message: { content: 'Summary', role: 'assistant' }, finish_reason: 'stop' }] }));
    response.writeHead(200, { 'Content-Type': 'text/event-stream' });
    const events = [{ choices: [{ delta: { content: 'Olá ' } }] }, { choices: [{ delta: { tool_calls: [{ index: 0, id: 'call1', function: { name: 'tool', arguments: '{"x":' } }] } }] }, { choices: [{ delta: { tool_calls: [{ index: 0, function: { arguments: '1}' } }] }, finish_reason: 'tool_calls' }] }];
    const bytes = Buffer.from(events.map(event => `data: ${JSON.stringify(event)}\r\n\r\n`).join('') + 'data: [DONE]\n\n');
    for (let i = 0; i < bytes.length; i += 3) response.write(bytes.subarray(i, i + 3)); response.end();
  });
  try {
    const provider = new Provider(config(new Store(), app.endpoint)); assert.deepEqual(await provider.models(), ['test-model']);
    let delta = ''; const result = await provider.complete([{ role: 'user', content: 'Hi' }], [], { onDelta: text => { delta += text; } });
    assert.equal(delta, 'Olá '); assert.equal(result.message.tool_calls[0].function.arguments, '{"x":1}'); assert.equal((await provider.complete([], [], { stream: false })).message.content, 'Summary');
  } finally { await app.close(); }
});

test('agent denies unapproved writes and persists valid tool replies', async () => {
  const store = new Store(), value = config(store); const mcp = new MCP(value); await mcp.connect(); let requests = 0;
  const app = await server(async (request, response) => {
    const data = await body(request); let message;
    if (++requests === 1) message = { role: 'assistant', content: '', tool_calls: [{ id: 'write-call', type: 'function', function: { name: find(mcp, 'write_file').publicName, arguments: JSON.stringify({ path: 'denied.txt', content: 'no' }) } }] };
    else { assert.match(data.messages.at(-1).content, /denied/); message = { role: 'assistant', content: 'Write was denied.' }; }
    response.setHeader('Content-Type', 'application/json'); response.end(JSON.stringify({ choices: [{ message, finish_reason: 'stop' }], usage: { total_tokens: 50 } }));
  });
  try {
    value.profiles.lmstudio.endpoint = app.endpoint; const chat = store.newChat(temp, 'lmstudio', 'test'); let approvals = 0;
    await new Agent(store, value, mcp, { approve: async () => { approvals++; return false; } }).run(chat, 'Write a file');
    assert.equal(approvals, 1); assert.equal(fs.existsSync(path.join(temp, 'denied.txt')), false); assert.equal(chat.messages.at(-1).content, 'Write was denied.'); assert.equal(store.loadChat(chat.id).usage.total_tokens, 50);
  } finally { await mcp.close(); await app.close(); }
});

test('compaction keeps full history and a complete latest tool turn', async () => {
  const store = new Store(), value = config(store); value.mcpServers = {}; const mcp = new MCP(value), chat = store.newChat(temp, 'lmstudio', 'test');
  chat.messages = [{ role: 'user', content: 'Old request' }, { role: 'assistant', content: 'Old result' }, { role: 'user', content: 'Latest request' }, { role: 'assistant', content: '', tool_calls: [{ id: 'c', type: 'function', function: { name: 'x', arguments: '{}' } }] }, { role: 'tool', tool_call_id: 'c', content: 'Latest result' }];
  const agent = new Agent(store, value, mcp); agent.provider.complete = async () => ({ message: { content: 'Preserved summary' } }); await agent.compact(chat, undefined, true);
  assert.equal(chat.messages.length, 5); assert.equal(chat.compacted, 2); assert.equal(agent.context(chat).length, 4); assert.match(agent.context(chat)[0].content, /Preserved summary/);
  assert.equal(store.loadChat(chat.id).messages.length, 5); assert.ok(estimate(agent.context(chat)) > 0);
});

test('cancellation stops a provider stream and shell process group', async () => {
  const app = await server(async (request, response) => { await body(request); response.writeHead(200, { 'Content-Type': 'text/event-stream' }); response.write('data: {"choices":[{"delta":{"content":"Start"}}]}\n\n'); });
  try {
    const controller = new AbortController(), provider = new Provider(config(new Store(), app.endpoint));
    const pending = provider.complete([], [], { signal: controller.signal, onDelta: () => controller.abort(new Error('Stop')) }); await assert.rejects(pending, /Stop|abort/);
    const control = new AbortController(), running = shell('sleep 20', temp, 30, control.signal); setTimeout(() => control.abort(), 30); assert.match(await running, /Cancelled/);
  } finally { await app.close(); }
});

test('timezone cron, background runs, unattended denial, saved results', async () => {
  assert.equal(nextRun('0 9 * * 1-5', 'Europe/Lisbon', new Date('2026-10-04T12:00:00Z')), '2026-10-05T08:00:00.000Z');
  assert.throws(() => nextRun('0 0 9 * * *', 'UTC'), /five-field/); assert.throws(() => nextRun('* * * * *', 'Fake/Zone'), /time zone/i);
  const store = new Store(), scheduler = new Scheduler(store), value = config(store), mcp = new MCP(value); await mcp.connect(); let mutate = false;
  const app = await server(async (request, response) => {
    await body(request); const message = mutate ? { role: 'assistant', content: '', tool_calls: [{ id: 'c', type: 'function', function: { name: find(mcp, 'write_file').publicName, arguments: '{"path":"unattended.txt","content":"no"}' } }] } : { role: 'assistant', content: 'Scheduled result' };
    response.end(JSON.stringify({ choices: [{ message, finish_reason: 'stop' }] }));
  });
  try {
    value.profiles.lmstudio.endpoint = app.endpoint; store.saveConfig(value);
    const task = scheduler.save({ name: 'Review', prompt: 'Review files', cron: '* * * * *', timezone: 'UTC', allowedTools: [] }, value, mcp);
    assert.equal((await scheduler.run(task.id)).messages.at(-1).content, 'Scheduled result'); assert.equal(store.tasks().find(item => item.id === task.id).lastStatus, 'completed');
    mutate = true; await assert.rejects(scheduler.run(task.id), /requires approval/); const failed = store.tasks().find(item => item.id === task.id);
    assert.equal(failed.lastStatus, 'failed'); assert.match(failed.lastError, /requires approval/); assert.equal(fs.existsSync(path.join(temp, 'unattended.txt')), false); assert.equal(store.loadChat(failed.lastChat).messages.at(-1).role, 'tool'); scheduler.remove(task.id);
  } finally { await mcp.close(); await app.close(); }
});

test('free search extracts titles, source links and snippets', () => {
  const html = '<a class="result__a" href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2F">Example &amp; page</a><a class="result__snippet">A <b>useful</b> snippet.</a>';
  assert.deepEqual(parseSearch(html, 5), [{ title: 'Example & page', url: 'https://example.com/', snippet: 'A useful snippet.' }]);
});

test('headless TUI renders tabs, edits messages and saves dialogs', async () => {
  const blessed = require('blessed'), { TUI } = require('../tui.cjs'), { terminalOptions } = require('../terminal.cjs'); const input = new PassThrough(); input.isTTY = true; input.setRawMode = () => {};
  let terminalOutput = '';
  const output = new Writable({ write(chunk, encoding, done) { terminalOutput += chunk.toString(); done(); } }); output.isTTY = true; output.columns = 124; output.rows = 38;
  const screen = blessed.screen({ input, output, ...terminalOptions('foot'), fullUnicode: true, smartCSR: true });
  // Reproduce Foot's previous eight-color fallback; verify actual emitted colors.
  screen.tput.colors = 8;
  const previousColorTerm = process.env.COLORTERM; process.env.COLORTERM = 'truecolor';
  const store = new Store(), value = config(store), ui = new TUI(store, value, { screen });
  if (previousColorTerm === undefined) delete process.env.COLORTERM; else process.env.COLORTERM = previousColorTerm;
  try {
    await ui.ready;
    for (let tab = 0; tab < 4; tab++) {
      ui.selectTab(tab); assert.equal(ui.tab, tab);
      if (tab === 1) { const list = ui.body.children.find(widget => widget.type === 'list'); list.select(1); screen.render(); }
      const text = screen.lines.map(row => row.map(cell => cell[1]).join('')).join('\n');
      assert.match(text, /Views/); assert.match(text, /Actions/); assert.match(text, /F5 Help/); assert.match(text, /[─│┌┐└┘]/);
      assert.doesNotMatch(text, /Local AI Agent|Ctrl\+Q quit/);
      assert.doesNotMatch(text, /Connected tools, resources and prompts|Choose a task to see|Agent preferences|Saved privately in/);
      fs.writeFileSync(`/tmp/dotfiles-agent-review-tab-${tab}.json`, JSON.stringify(screen.lines));
    }
    const action = ui.focusables(ui.body).find(widget => widget.type === 'button'); action.focus(); screen.render(); screen.program.flush();
    assert.match(terminalOutput, /38;2;255;190;111/); assert.match(terminalOutput, /48;2;255;190;111/);
    assert.match(terminalOutput, /48;2;255;163;72/); assert.match(terminalOutput, /38;2;170;170;170/);
    assert.match(terminalOutput, /48;2;34;34;38/);
    const pending = ui.form('Test setting', [{ key: 'value', label: 'Value', value: 'abc' }]); screen.focused.emit('keypress', 'x', { name: 'x' });
    screen.emit('key C-s'); assert.equal((await pending).value, 'abcx');
    const multiline = ui.form('Test multiline setting', [{ key: 'value', label: 'Value', value: 'first', multiline: true }]);
    input.write('\rsecond'); assert.ok(ui.modal); screen.emit('key C-s'); assert.equal((await multiline).value, 'first\nsecond');
    ui.selectTab(0); ui.composer.widget.emit('keypress', 'hello', { name: 'h' }); assert.equal(ui.composer.value, 'hello');
    input.write('\x1b[20'); input.write('0~\n\tcode {braces}\n\x1b[201~');
    assert.equal(ui.composer.value, 'hello\n\tcode {braces}\n'); assert.equal(screen.focused, ui.composer.widget);
    let sent; const run = ui.run; ui.run = async prompt => { sent = prompt; };
    // Real terminal bytes, including split CSI-u, plain Return and xterm keys.
    ui.composer.set('first'); input.write('\x1b[13;'); input.write('2u'); input.write('second?');
    assert.equal(ui.composer.value, 'first\nsecond?'); assert.equal(ui.modal, null); assert.equal(sent, undefined);
    input.write('\r'); assert.equal(sent, 'first\nsecond?'); assert.equal(ui.composer.value, '');
    sent = undefined; input.write('third'); input.write('\x1b[27;2;13~'); input.write('fourth');
    assert.equal(ui.composer.value, 'third\nfourth'); input.write('\x1b[13u'); assert.equal(sent, 'third\nfourth');
    sent = undefined; input.write('\x1b[200~pasted\nlines\x1b[201~'); assert.equal(sent, undefined);
    assert.equal(ui.composer.value, 'pasted\nlines'); input.write('\x1b[115;5u'); assert.equal(sent, 'pasted\nlines');
    ui.run = run;
    input.write('?'); assert.equal(ui.modal, ui.helpModal); assert.ok(ui.helpModal);
    assert.match(screen.lines.map(row => row.map(cell => cell[1]).join('')).join('\n'), /Shift\+Enter adds a line/);
    fs.writeFileSync('/tmp/dotfiles-agent-review-help.json', JSON.stringify(screen.lines));
    input.write('?'); await new Promise(setImmediate); assert.equal(ui.modal, null); assert.equal(screen.focused, ui.composer.widget);
    ui.selectTab(1); const help = ui.selectTab(4); assert.equal(ui.tab, 1); assert.ok(ui.helpModal);
    screen.emit('key escape'); await help; assert.equal(ui.tab, 1);
    ui.selectTab(3);
    const original = structuredClone(ui.config);
    let edit = ui.editSetting(['permissions', 'Tool permissions']);
    const picker = ui.modal.children.find(widget => widget.type === 'list'); picker.select(2); picker.emit('select', null, 2);
    await new Promise(setImmediate);
    const enable = ui.modal.children.find(widget => widget.type === 'button' && widget.content === 'Enable Auto');
    assert.ok(enable); enable.emit('press'); await edit; assert.equal(new Store().config().permissions, 'auto');
    edit = ui.editSetting(['permissions', 'Tool permissions']); assert.equal(ui.modal.children.find(widget => widget.type === 'list').selected, 2);
    screen.emit('key escape'); await edit; assert.equal(ui.config.permissions, 'auto');
    store.saveConfig(original); ui.config = original; ui.selectTab(0);
    fs.writeFileSync('/tmp/dotfiles-agent-review-screen.json', JSON.stringify(screen.lines));
  } finally {
    await ui.mcp.close(); screen.destroy(); input.destroy(); output.destroy();
    assert.ok(terminalOutput.indexOf('\x1b[<1u') < terminalOutput.indexOf('\x1b[?1049l'), 'Restore keyboard mode before returning to the shell buffer');
  }
});

test('Seth shows the Linux username and clears progress after finished, failed or stopped responses', async () => {
  const blessed = require('blessed'), { TUI } = require('../tui.cjs');
  const input = new PassThrough(); input.isTTY = true; input.setRawMode = () => {};
  const output = new Writable({ write(chunk, encoding, done) { done(); } }); output.isTTY = true; output.columns = 124; output.rows = 38;
  const screen = blessed.screen({ input, output, terminal: 'xterm-256color', fullUnicode: true, smartCSR: true });
  const store = new Store(), value = config(store); value.mcpServers = {};
  const previousUser = process.env.USER; process.env.USER = 'test-linux-user';
  const ui = new TUI(store, value, { screen });
  if (previousUser === undefined) delete process.env.USER; else process.env.USER = previousUser;
  const status = () => blessed.stripTags(ui.status.content);
  try {
    await ui.ready;
    const makeAgent = ui.makeAgent.bind(ui); let complete;
    ui.makeAgent = () => { const agent = makeAgent(); agent.provider.complete = (...args) => complete(...args); return agent; };
    complete = async (messages, tools, options) => {
      assert.match(messages[0].content, /Agent name: Seth/);
      assert.equal(ui.busy, true); assert.match(status(), /^Thinking/);
      options.onDelta('Hello from Seth.');
      assert.match(blessed.stripTags(ui.transcript()), /\nTest-linux-user\nHello Seth/);
      assert.match(blessed.stripTags(ui.transcript()), /\nSeth\nHello from Seth\./);
      return { message: { role: 'assistant', content: 'Hello from Seth.' }, usage: {}, finish: 'stop' };
    };
    await ui.run('Hello Seth');
    assert.equal(ui.busy, false); assert.equal(ui.partial, ''); assert.match(status(), /^Ready/); assert.doesNotMatch(status(), /Thinking|Using/);
    const saved = store.loadChat(ui.chat.id); await ui.loadChat(saved.id);
    assert.match(blessed.stripTags(ui.transcript()), /\nSeth\nHello from Seth\./);
    complete = async () => ({ message: { role: 'assistant', content: 'Partial reply.' }, usage: {}, finish: 'length' });
    await ui.run('Keep going'); assert.match(status(), /output limit/); assert.doesNotMatch(status(), /Thinking/);
    complete = async () => { throw new Error('Provider disconnected'); };
    await ui.run('Try again'); assert.match(status(), /^Provider disconnected/); assert.equal(ui.busy, false);
    complete = async (messages, tools, options) => { ui.controller.abort(new Error('Cancelled')); options.signal.throwIfAborted(); };
    await ui.run('Stop this reply'); assert.match(status(), /^Stopped/); assert.doesNotMatch(status(), /Thinking/);
    complete = async () => ({ message: { role: 'assistant', content: 'Recovered.' }, usage: {}, finish: 'stop' });
    await ui.run('Resume normally'); assert.match(status(), /^Ready/);
  } finally { await ui.mcp.close(); screen.destroy(); input.destroy(); output.destroy(); }
});

test('tool rows are muted, collapsed and clickable across wrapping, streaming and chat changes', async () => {
  const blessed = require('blessed'), { TUI } = require('../tui.cjs'), { SLOTS } = require('../terminal.cjs');
  const input = new PassThrough(); input.isTTY = true; input.setRawMode = () => {};
  const output = new Writable({ write(chunk, encoding, done) { done(); } }); output.isTTY = true; output.columns = 100; output.rows = 32;
  const screen = blessed.screen({ input, output, terminal: 'xterm-256color', fullUnicode: true, smartCSR: true });
  const store = new Store(), value = config(store); value.mcpServers = {};
  const ui = new TUI(store, value, { screen });
  const call = (id, name, args) => ({ id, type: 'function', function: { name, arguments: args } });
  const text = () => ui.log.getText();
  const click = (key, continuation = false) => {
    const logical = [...ui.toolRows].find(([, entry]) => entry === key)[0], rows = ui.log._clines.ftor[logical];
    const row = rows[continuation && rows.length > 1 ? 1 : 0];
    ui.log.setScroll(Math.max(0, row - 2)); screen.render();
    const pos = ui.log.lpos, x = pos.xi + ui.log.ileft + 1, y = pos.yi + ui.log.itop + row - pos.base;
    input.write(`\x1b[<0;${x + 1};${y + 1}M`); input.write(`\x1b[<0;${x + 1};${y + 1}m`);
  };
  const mutedText = snippet => {
    const row = screen.lines.find(row => row.map(cell => cell[1]).join('').includes(snippet));
    assert.ok(row, `Visible: ${snippet}`);
    for (const [attr, char] of row.slice(ui.log.lpos.xi + ui.log.ileft, ui.log.lpos.xl - ui.log.iright - 1)) {
      if (char.trim()) assert.equal((attr >> 9) & 511, SLOTS.muted);
    }
  };
  try {
    await ui.ready;
    ui.chat.messages = [
      { role: 'user', content: 'Please inspect these files.\n' + 'long wrapped text '.repeat(60) },
      { role: 'assistant', content: '', tool_calls: [call('reused', 'filesystem__read_file_with_a_very_long_name', '{"path":"private-note.txt"}')] },
      { role: 'tool', tool_call_id: 'reused', content: 'PRIVATE_RESULT\n' + 'Result details '.repeat(100) },
      { role: 'assistant', content: 'First inspection finished.' },
      { role: 'assistant', content: '', tool_calls: [call('reused', 'filesystem__read_file', '{"path":"second.txt"}')] },
      { role: 'tool', tool_call_id: 'reused', content: 'SECOND_RESULT' },
      { role: 'assistant', content: '', tool_calls: [call('pending', 'web__search', '{"query":"test"}')] },
    ];
    store.saveChat(ui.chat); ui.selectTab(0);
    assert.match(text(), /▶.*filesystem__read_file.*done/); assert.match(text(), /▶ web__search · pending/);
    assert.doesNotMatch(text(), /private-note\.txt|PRIVATE_RESULT|SECOND_RESULT|Arguments/);
    const [first, second, pending] = [...ui.toolRows.values()];
    click(first, true); assert.match(text(), /▼.*filesystem__read_file/); assert.match(text(), /private-note\.txt|PRIVATE_RESULT/); assert.doesNotMatch(text(), /SECOND_RESULT/);
    mutedText('Arguments');
    const resultLine = ui.log._clines.findIndex(line => line.includes('PRIVATE_RESULT'));
    ui.log.setScroll(Math.max(0, resultLine - 1)); screen.render(); mutedText('PRIVATE_RESULT');
    click(first, true); assert.doesNotMatch(text(), /PRIVATE_RESULT|private-note\.txt/);
    click(second); assert.match(text(), /SECOND_RESULT/); assert.doesNotMatch(text(), /PRIVATE_RESULT/);
    const log = ui.log, pos = log.lpos;
    log.emit('click', { x: pos.xi, y: pos.yi, button: 'left' }); assert.match(text(), /SECOND_RESULT/);
    ui.chat.messages.push({ role: 'tool', tool_call_id: 'pending', content: 'Tool error: Search failed.' });
    ui.makeAgent().onEvent({ type: 'result', text: 'Tool error: Search failed.' });
    assert.match(text(), /web__search · failed/); assert.doesNotMatch(text(), /Search failed\./); assert.match(text(), /SECOND_RESULT/);
    click(pending); assert.match(text(), /Search failed\./);
    screen.program.cols = 74; screen.alloc(); screen.emit('resize'); screen.render();
    click(first, true); assert.match(text(), /PRIVATE_RESULT/); assert.match(text(), /SECOND_RESULT/);
    ui.selectTab(3); ui.selectTab(0); assert.match(text(), /PRIVATE_RESULT/);
    const previous = ui.chat; ui.chat = store.newChat(temp, value.profile, 'test'); ui.chat.messages = structuredClone(previous.messages); ui.selectTab(0);
    assert.doesNotMatch(text(), /PRIVATE_RESULT|SECOND_RESULT|Arguments/);
    await ui.loadChat(previous.id); assert.match(text(), /PRIVATE_RESULT/);
    assert.match(store.loadChat(previous.id).messages[2].content, /PRIVATE_RESULT/);
    // A compact review example, using the same real mouse path as above.
    screen.program.cols = 124; screen.program.rows = 38; screen.alloc(); screen.emit('resize');
    ui.username = 'trude'; ui.chat = store.newChat(temp, value.profile, 'test'); ui.chat.title = 'Inspect the agent settings';
    ui.mcp.tools.set('read_file', { serverName: 'filesystem', name: 'read_file' });
    ui.mcp.tools.set('search', { serverName: 'web', name: 'search' });
    ui.chat.messages = [
      { role: 'user', content: 'Check my agent settings and look up the provider documentation.' },
      { role: 'assistant', content: 'I’ll read the settings and search for the documentation.', tool_calls: [call('review-file', 'read_file', '{"path":"settings.json"}'), call('review-web', 'search', '{"query":"LM Studio documentation"}')] },
      { role: 'tool', tool_call_id: 'review-file', content: '{\n  "provider": "lmstudio",\n  "permissions": "auto",\n  "contextWindow": 32768\n}' },
      { role: 'tool', tool_call_id: 'review-web', content: 'LM Studio documentation\nhttps://lmstudio.ai/docs\nGuides for local models and the OpenAI-compatible API.' },
      { role: 'assistant', content: 'You’re using LM Studio with Auto permissions and a context window of 32,768 tokens.' },
    ];
    ui.selectTab(0); ui.log.setScroll(0); screen.render();
    fs.writeFileSync('/tmp/dotfiles-agent-review-tools-collapsed.json', JSON.stringify(screen.lines));
    click([...ui.toolRows.values()][0]);
    fs.writeFileSync('/tmp/dotfiles-agent-review-tools-expanded.json', JSON.stringify(screen.lines));
  } finally { await ui.mcp.close(); screen.destroy(); input.destroy(); output.destroy(); }
});

test('saved auto permissions allow repeated tools across chats and restarts', async () => {
  const store = new Store(), original = config(store), value = structuredClone(original); value.permissions = 'auto'; store.saveConfig(value);
  const mcp = new MCP(value); await mcp.connect();
  try {
    for (let session = 0; session < 2; session++) {
      const config = new Store().config(), chat = store.newChat(temp, config.profile, 'test');
      const agent = new Agent(store, config, mcp, { approve: async () => { assert.fail('Auto must not prompt for each tool'); } });
      let step = 0;
      agent.provider.complete = async () => ++step <= 2 ? { message: { role: 'assistant', content: '', tool_calls: [{ id: `auto-${step}`, type: 'function', function: { name: find(mcp, 'write_file').publicName, arguments: JSON.stringify({ path: `auto-${session}-${step}.txt`, content: 'remembered' }) } }] }, usage: {} } : { message: { role: 'assistant', content: 'Finished' }, usage: {}, finish: 'stop' };
      await agent.run(chat, 'Write two files');
      for (let file = 1; file <= 2; file++) assert.equal(fs.readFileSync(path.join(temp, `auto-${session}-${file}.txt`), 'utf8'), 'remembered');
    }
  } finally { await mcp.close(); store.saveConfig(original); }
});

test('automatic compaction fits a small context and preserves old messages', async () => {
  const store = new Store(), value = config(store); value.mcpServers = {}; value.contextWindow = 8192; value.maxTokens = 1024;
  const chat = store.newChat(temp, 'lmstudio', 'test');
  chat.messages = [{ role: 'user', content: 'Earlier request ' + 'x'.repeat(6000) }, { role: 'assistant', content: 'Earlier reply ' + 'y'.repeat(6000) }];
  const agent = new Agent(store, value, new MCP(value)); let summarized = 0;
  agent.provider.complete = async (messages, tools, options) => {
    assert.ok(estimate([messages, tools]) < value.contextWindow);
    if (options.stream === false) { summarized++; return { message: { role: 'assistant', content: 'Earlier goal and result preserved.' } }; }
    return { message: { role: 'assistant', content: 'Continued successfully.' }, usage: {}, finish: 'stop' };
  };
  await agent.run(chat, 'Continue with the next task.');
  assert.ok(summarized > 0); assert.equal(chat.compacted, 2); assert.equal(chat.messages[0].content.length, 6016); assert.equal(chat.messages.at(-1).content, 'Continued successfully.');
});

test('agent can approve a file write and learn from its MCP result', async () => {
  const store = new Store(), value = config(store), mcp = new MCP(value); await mcp.connect();
  const chat = store.newChat(temp, 'lmstudio', 'test'); const agent = new Agent(store, value, mcp, { approve: async () => true }); let step = 0;
  agent.provider.complete = async messages => {
    if (++step === 1) return { message: { role: 'assistant', content: '', tool_calls: [{ id: 'approved', type: 'function', function: { name: find(mcp, 'write_file').publicName, arguments: JSON.stringify({ path: 'approved.txt', content: 'Approved through the agent.' }) } }] }, usage: {} };
    assert.match(messages.at(-1).content, /Wrote.*approved.txt/); return { message: { role: 'assistant', content: 'File written.' }, usage: {}, finish: 'stop' };
  };
  try { await agent.run(chat, 'Write the approved file'); assert.equal(fs.readFileSync(path.join(temp, 'approved.txt'), 'utf8'), 'Approved through the agent.'); }
  finally { await mcp.close(); }
});

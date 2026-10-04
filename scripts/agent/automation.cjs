'use strict';
const fs = require('node:fs');
const path = require('node:path');
const { execFile } = require('node:child_process');
const { promisify } = require('node:util');
const { randomUUID } = require('node:crypto');
const { CronExpressionParser } = require('cron-parser');
const { MCP } = require('./mcp.cjs');
const { Agent } = require('./agent.cjs');
const exec = promisify(execFile);
function nextRun(cron, timezone, from = new Date()) {
  new Intl.DateTimeFormat('en', { timeZone: timezone }).format(from);
  if (cron.trim().split(/\s+/).length !== 5) throw new Error('Use a five-field cron expression: minute hour day month weekday.');
  return CronExpressionParser.parse(cron, { tz: timezone, currentDate: from }).next().toISOString();
}
const nativeTools = [
  { type: 'function', function: { name: 'automations_list', description: 'List the agent’s scheduled tasks and their last results.', parameters: { type: 'object', properties: {} } } },
  { type: 'function', function: { name: 'automations_create', description: 'Schedule a recurring task. Defaults to read-only tools. Requires approval. Five-field cron, e.g. 0 9 * * 1-5.', parameters: { type: 'object', properties: { name: { type: 'string' }, prompt: { type: 'string' }, cron: { type: 'string' }, timezone: { type: 'string' }, allowedTools: { type: 'array', items: { type: 'string' } } }, required: ['name', 'prompt', 'cron'] } } },
  { type: 'function', function: { name: 'automations_update', description: 'Update a scheduled task. Requires approval.', parameters: { type: 'object', properties: { id: { type: 'string' }, name: { type: 'string' }, prompt: { type: 'string' }, cron: { type: 'string' }, timezone: { type: 'string' }, enabled: { type: 'boolean' }, allowedTools: { type: 'array', items: { type: 'string' } } }, required: ['id'] } } },
  { type: 'function', function: { name: 'automations_delete', description: 'Delete a scheduled task. Requires approval.', parameters: { type: 'object', properties: { id: { type: 'string' } }, required: ['id'] } } },
];
class Scheduler {
  constructor(store) { this.store = store; }
  change(callback) {
    const unlock = this.store.lock('tasks');
    try { const tasks = this.store.tasks(), result = callback(tasks); this.store.saveTasks(tasks); return result; }
    finally { unlock(); }
  }
  save(input, config, mcp, id) {
    const allowedTools = input.allowedTools || [];
    if (!Array.isArray(allowedTools) || allowedTools.some(name => typeof name !== 'string' || !mcp.tools.has(name))) throw new Error('Allowed tools must name connected MCP tools.');
    for (const key of ['name', 'prompt', 'cron']) if (typeof input[key] !== 'string' || !input[key].trim()) throw new Error(`${key} is required.`);
    if (input.name.length > 120 || input.prompt.length > 12000) throw new Error('Name or prompt is too long.');
    if (input.enabled !== undefined && typeof input.enabled !== 'boolean') throw new Error('enabled must be a boolean.');
    const timezone = input.timezone || Intl.DateTimeFormat().resolvedOptions().timeZone;
    const task = { ...input, allowedTools, timezone, enabled: input.enabled ?? true, nextRun: nextRun(input.cron, timezone), workspace: config.workspace, profile: config.profile, model: config.profiles[config.profile].model };
    if (!task.model) throw new Error('Select a model before scheduling a task.');
    return this.change(tasks => {
      if (id) {
        const index = tasks.findIndex(item => item.id === id); if (index < 0) throw new Error('Task no longer exists.');
        tasks[index] = { ...tasks[index], ...task }; return tasks[index];
      }
      const item = { ...task, id: randomUUID(), lastStatus: 'never run' }; tasks.push(item); return item;
    });
  }
  update(id, patch) {
    return this.change(tasks => {
      const task = tasks.find(item => item.id === id); if (!task) throw new Error('Task no longer exists.'); Object.assign(task, patch); return task;
    });
  }
  remove(id) {
    return this.change(tasks => { const index = tasks.findIndex(task => task.id === id); if (index < 0) throw new Error('Task no longer exists.'); tasks.splice(index, 1); return 'Task deleted.'; });
  }
  async native(name, args, config, mcp) {
    if (name === 'automations_list') return JSON.stringify(this.store.tasks(), null, 2);
    if (name === 'automations_delete') return this.remove(args.id);
    if (name === 'automations_create') {
      const task = this.save(args, config, mcp);
      return JSON.stringify(task) + '\nTask saved. Enable the scheduler in the Automation tab for background execution.';
    }
    if (name === 'automations_update') {
      const task = this.store.tasks().find(item => item.id === args.id); if (!task) throw new Error('Unknown task.');
      return JSON.stringify(this.save({ ...task, ...args }, config, mcp, args.id));
    }
    throw new Error('Unknown native tool.');
  }
  async timerStatus() {
    try { const result = await exec('systemctl', ['--user', 'is-enabled', 'dotfiles-agent.timer'], { timeout: 3000 }); return result.stdout.trim(); }
    catch { return 'disabled / unavailable'; }
  }
  async enable() {
    const folder = path.join(process.env.XDG_CONFIG_HOME || path.join(require('node:os').homedir(), '.config'), 'systemd/user');
    fs.mkdirSync(folder, { recursive: true });
    // Install only our named units. A direct checkout is supported without the desktop installer.
    for (const name of ['dotfiles-agent.service', 'dotfiles-agent.timer']) {
      const file = path.join(folder, name);
      if (!fs.existsSync(file)) {
        let source = fs.readFileSync(path.join(__dirname, '../../configs/systemd/user', name), 'utf8');
        if (name.endsWith('.service')) {
          const launcher = path.join(__dirname, '../tui/agent-tui');
          const quoted = launcher.replace(/\\/g, '\\\\').replace(/"/g, '\\"').replace(/%/g, '%%');
          source = source.replace('ExecStart=%h/.local/bin/dotfiles-agent-tui --run-due', `ExecStart="${quoted}" --run-due`);
        }
        fs.writeFileSync(file, source, { mode: 0o600, flag: 'wx' });
      }
    }
    await exec('systemctl', ['--user', 'daemon-reload'], { timeout: 5000 });
    await exec('systemctl', ['--user', 'enable', '--now', 'dotfiles-agent.timer'], { timeout: 5000 });
  }
  async disable() { await exec('systemctl', ['--user', 'disable', '--now', 'dotfiles-agent.timer'], { timeout: 5000 }); }
  async run(id, { signal } = {}) {
    const unlock = this.store.lock(`task-${id}`);
    let task, chat, mcp;
    try {
      task = this.change(tasks => {
        const item = tasks.find(item => item.id === id); if (!item) throw new Error('Task no longer exists.');
        item.nextRun = nextRun(item.cron, item.timezone); item.lastRun = new Date().toISOString(); item.lastStatus = 'running'; item.lastError = ''; return structuredClone(item);
      });
      const config = this.store.config();
      config.workspace = fs.realpathSync(task.workspace); config.profile = task.profile;
      config.profiles[task.profile].model = task.model;
      chat = this.store.newChat(config.workspace, task.profile, task.model); chat.title = `Scheduled: ${task.name}`; this.store.saveChat(chat);
      this.update(id, { lastChat: chat.id });
      mcp = new MCP(config); await mcp.connect(signal);
      // Unattended runs never execute native scheduling mutations or prompt for approval.
      const agent = new Agent(this.store, config, mcp, { nativeTools: [nativeTools[0]], nativeCall: () => JSON.stringify(this.store.tasks()) });
      await agent.run(chat, task.prompt, { signal, background: { allowedTools: task.allowedTools } });
      this.update(id, { lastStatus: 'completed', lastError: '' });
      return chat;
    } catch (error) {
      if (task) { try { this.update(id, { lastStatus: 'failed', lastError: error.message, ...(chat ? { lastChat: chat.id } : {}) }); } catch {} }
      throw error;
    } finally { await mcp?.close(); unlock(); }
  }
  async runDue(signal) {
    const unlock = this.store.lock('scheduler'); const results = [];
    try {
      for (const task of this.store.tasks()) {
        if (!task.enabled || Date.parse(task.nextRun) > Date.now()) continue;
        signal?.throwIfAborted();
        try { await this.run(task.id, { signal }); results.push(`${task.name}: completed`); }
        catch (error) { results.push(`${task.name}: ${error.message}`); }
      }
      return results;
    } finally { unlock(); }
  }
}
module.exports = { Scheduler, nativeTools, nextRun };

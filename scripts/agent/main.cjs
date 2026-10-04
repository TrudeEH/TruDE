'use strict';
const fs = require('node:fs');
const { Store } = require('./store.cjs');
async function main() {
  if (Number(process.versions.node.split('.')[0]) < 20) throw new Error('Node.js 20 or newer is required.');
  if (process.argv.includes('--help')) {
    console.log('Seth\n\nUsage: agent-tui [--workspace DIR] [--run-due | --run-task ID | --check]\n\nStart the TUI to configure providers, models, MCP servers and automations.\nEnter sends, Shift+Enter adds a line, Escape stops, F1–F4 select tabs, F5 opens help, Ctrl+Q quits.\nDependencies: run scripts/agent/setup.sh or the dotfiles installer.'); return;
  }
  const store = new Store(), config = store.config();
  const workspace = process.argv.indexOf('--workspace');
  if (workspace >= 0) { if (!process.argv[workspace + 1]) throw new Error('--workspace needs a directory.'); config.workspace = fs.realpathSync(process.argv[workspace + 1]); }
  if (process.argv.includes('--run-due') || process.argv.includes('--run-task')) {
    const { Scheduler } = require('./automation.cjs'); const scheduler = new Scheduler(store), controller = new AbortController();
    process.once('SIGTERM', () => controller.abort()); process.once('SIGINT', () => controller.abort());
    const index = process.argv.indexOf('--run-task');
    if (index >= 0) { if (!process.argv[index + 1]) throw new Error('--run-task needs an ID.'); await scheduler.run(process.argv[index + 1], { signal: controller.signal }); }
    else console.log((await scheduler.runDue(controller.signal)).join('\n'));
    return;
  }
  if (process.argv.includes('--check')) {
    const { MCP } = require('./mcp.cjs'); const mcp = new MCP(config);
    try { await mcp.connect(); for (const [name, status] of mcp.status) { console.log(`${name}: ${status.state} (${status.tools} tools) ${status.era || ''}`); if (status.state === 'error') { console.error(status.details); process.exitCode = 1; } } }
    finally { await mcp.close(); } return;
  }
  if (!process.stdin.isTTY || !process.stdout.isTTY) throw new Error('Open this agent in a terminal, or use --help / --check / --run-due.');
  const { TUI } = require('./tui.cjs'); const ui = new TUI(store, config);
  ui.ready.catch(error => ui.setNotice(error.message));
  process.once('SIGTERM', () => ui.quit());
}
main().catch(error => {
  if (error.code === 'MODULE_NOT_FOUND') console.error('Agent dependencies are missing. Run scripts/agent/setup.sh or the dotfiles installer.');
  else console.error(error.message);
  process.exitCode = 1;
});

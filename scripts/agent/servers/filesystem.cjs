'use strict';
const fs = require('node:fs/promises');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { randomUUID } = require('node:crypto');
const z = require('zod/v4');
const { McpServer } = require('@modelcontextprotocol/server');
const { serveStdio } = require('@modelcontextprotocol/server/stdio');
const { dirs, atomic, read } = require('../store.cjs');

async function resolveInside(root, value, creating = false) {
  const candidate = path.resolve(root, value);
  const inside = target => target === root || target.startsWith(root === '/' ? root : root + path.sep);
  if (!inside(candidate)) throw new Error('Path is outside the workspace.');
  let real;
  try { real = await fs.realpath(candidate); }
  catch (error) {
    if (!creating || error.code !== 'ENOENT') throw error;
    // Resolve the nearest existing ancestor, including symlinks, before creating.
    let parent = path.dirname(candidate), suffix = path.basename(candidate);
    while (true) {
      try { real = path.join(await fs.realpath(parent), suffix); break; }
      catch (missing) {
        if (missing.code !== 'ENOENT' || parent === path.dirname(parent)) throw missing;
        suffix = path.join(path.basename(parent), suffix); parent = path.dirname(parent);
      }
    }
    // Reject dangling symlinks, which realpath alone treats as missing.
    try { if ((await fs.lstat(candidate)).isSymbolicLink()) throw new Error('Dangling symlink is not writable.'); }
    catch (stat) { if (stat.code !== 'ENOENT') throw stat; }
  }
  if (!inside(real)) throw new Error('Symlink points outside the workspace.');
  return real;
}
async function shell(command, cwd, timeout, signal) {
  return new Promise((resolve, reject) => {
    const child = spawn('/bin/sh', ['-c', command], { cwd, detached: true, stdio: ['ignore', 'pipe', 'pipe'] });
    let output = '', stopped = '', killTimer;
    const kill = reason => { stopped = reason; try { process.kill(-child.pid, 'SIGTERM'); } catch {} killTimer = setTimeout(() => { try { process.kill(-child.pid, 'SIGKILL'); } catch {} }, 1000); };
    const cancel = () => kill('Cancelled');
    const timer = setTimeout(() => kill('Timed out'), timeout * 1000);
    signal?.addEventListener('abort', cancel, { once: true });
    if (signal?.aborted) cancel();
    const collect = chunk => { if (output.length < 24000) output += String(chunk).slice(0, 24000 - output.length); };
    child.stdout.on('data', collect); child.stderr.on('data', collect);
    const clean = () => { if (stopped) { try { process.kill(-child.pid, 'SIGKILL'); } catch {} } clearTimeout(timer); clearTimeout(killTimer); signal?.removeEventListener('abort', cancel); };
    child.on('error', error => { clean(); reject(error); });
    child.on('close', (code, sig) => { clean(); resolve(`${stopped || `Exit ${code ?? sig}`}\n${output}`); });
  });
}
async function createServer(workspace = process.env.AGENT_WORKSPACE || process.cwd()) {
  const root = await fs.realpath(workspace);
  const checkpointDir = path.join(dirs().state, 'checkpoints');
  const server = new McpServer({ name: 'workspace', version: '0.1.0' }, { instructions: `All file paths are relative to ${root}. Shell commands run there but are NOT sandboxed. Request approval for shell commands and changes. write_file and edit_file make checkpoints before changing files.` });
  const tool = (name, description, schema, safe, handler) => server.registerTool(name, { description, inputSchema: z.object(schema), annotations: { readOnlyHint: safe, destructiveHint: !safe, openWorldHint: name === 'shell' } }, async (args, ctx) => {
    try { ctx.signal?.throwIfAborted(); return { content: [{ type: 'text', text: String(await handler(args, ctx)) }] }; }
    catch (error) { return { isError: true, content: [{ type: 'text', text: error.message }] }; }
  });
  const fileSchema = { path: z.string().min(1) };
  const checkpoint = async file => {
    let content = null, mode = 0o600;
    try { const stat = await fs.stat(file); if (!stat.isFile() || stat.size > 2000000) throw new Error('Only text files up to 2 MB can be edited.'); content = await fs.readFile(file, 'utf8'); if (content.includes('\0')) throw new Error('Binary files cannot be edited.'); mode = stat.mode & 0o777; }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
    const id = randomUUID(); atomic(path.join(checkpointDir, `${id}.json`), { id, file, root, content, mode, created: new Date().toISOString() }); return id;
  };
  tool('list_directory', 'List workspace directory entries.', { path: z.string().default('.') }, true, async args => {
    const folder = await resolveInside(root, args.path);
    return (await fs.readdir(folder, { withFileTypes: true })).slice(0, 500).map(entry => `${entry.isDirectory() ? 'dir ' : entry.isSymbolicLink() ? 'link' : 'file'} ${entry.name}`).join('\n');
  });
  tool('read_file', 'Read a UTF-8 file, optionally a range of lines.', { ...fileSchema, start: z.number().int().min(1).default(1), lines: z.number().int().min(1).max(1000).default(200) }, true, async args => {
    const file = await resolveInside(root, args.path), stat = await fs.stat(file);
    if (!stat.isFile() || stat.size > 2000000) throw new Error('Only files up to 2 MB can be read.');
    const content = await fs.readFile(file, 'utf8'); if (content.includes('\0')) throw new Error('Binary file.');
    const lines = content.split('\n'); return lines.slice(args.start - 1, args.start - 1 + args.lines).map((line, i) => `${i + args.start}: ${line}`).join('\n').slice(0, 16000) + `\n[${lines.length} lines total]`;
  });
  tool('file_info', 'Inspect size, permissions and modification time.', fileSchema, true, async args => {
    const file = await resolveInside(root, args.path), stat = await fs.stat(file);
    return JSON.stringify({ path: file, bytes: stat.size, mode: (stat.mode & 0o777).toString(8), modified: stat.mtime, directory: stat.isDirectory() });
  });
  tool('search_files', 'Find names or literal text recursively. Skips symlinks, .git and node_modules.', { path: z.string().default('.'), query: z.string().min(1), content: z.boolean().default(false) }, true, async (args, ctx) => {
    const found = []; let visited = 0;
    const walk = async folder => {
      for (const entry of await fs.readdir(folder, { withFileTypes: true })) {
        ctx.signal?.throwIfAborted(); if (++visited > 10000 || found.length >= 100) return;
        if (entry.isSymbolicLink() || ['.git', 'node_modules'].includes(entry.name)) continue;
        const file = path.join(folder, entry.name), relative = path.relative(root, file);
        if (!args.content && relative.includes(args.query)) found.push(relative);
        if (entry.isDirectory()) await walk(file);
        else if (args.content && entry.isFile()) {
          try { if ((await fs.stat(file)).size > 500000) continue; const text = await fs.readFile(file, 'utf8'); if (text.includes('\0')) continue; text.split('\n').forEach((line, i) => { if (found.length < 100 && line.includes(args.query)) found.push(`${relative}:${i + 1}: ${line.slice(0, 250)}`); }); } catch {}
        }
      }
    };
    await walk(await resolveInside(root, args.path)); return found.join('\n') || 'No matches.';
  });
  tool('write_file', 'Write UTF-8 text with an automatic checkpoint. Use expected_content to prevent conflicting overwrites.', { ...fileSchema, content: z.string().max(2000000), expected_content: z.string().optional() }, false, async args => {
    const file = await resolveInside(root, args.path, true);
    if (args.expected_content !== undefined && await fs.readFile(file, 'utf8') !== args.expected_content) throw new Error('File changed since it was read.');
    const id = await checkpoint(file); await fs.mkdir(path.dirname(file), { recursive: true }); await fs.writeFile(file, args.content, { mode: 0o600 }); return `Wrote ${file}. Checkpoint: ${id}`;
  });
  tool('edit_file', 'Replace one exact occurrence of text, with a checkpoint and conflict detection.', { ...fileSchema, old_text: z.string().min(1), new_text: z.string() }, false, async args => {
    const file = await resolveInside(root, args.path), id = await checkpoint(file), text = await fs.readFile(file, 'utf8');
    if (text.split(args.old_text).length !== 2) throw new Error('old_text must match exactly once.');
    await fs.writeFile(file, text.replace(args.old_text, () => args.new_text)); return `Edited ${file}. Checkpoint: ${id}`;
  });
  tool('list_checkpoints', 'List recent file checkpoints in this workspace.', {}, true, async () => {
    let files; try { files = await fs.readdir(checkpointDir); } catch { return 'No checkpoints.'; }
    return files.filter(f => f.endsWith('.json')).map(f => read(path.join(checkpointDir, f), null)).filter(item => item?.root === root).sort((a, b) => b.created.localeCompare(a.created)).slice(0, 50).map(item => `${item.id} ${item.created} ${path.relative(root, item.file)}`).join('\n') || 'No checkpoints.';
  });
  tool('restore_checkpoint', 'Restore an earlier file version. Makes a checkpoint of the current version first.', { id: z.string().uuid() }, false, async args => {
    const saved = read(path.join(checkpointDir, `${args.id}.json`), null);
    if (!saved || saved.root !== root) throw new Error('Checkpoint does not belong to this workspace.');
    const file = await resolveInside(root, saved.file, true), id = await checkpoint(file);
    if (saved.content === null) await fs.unlink(file).catch(error => { if (error.code !== 'ENOENT') throw error; });
    else { await fs.mkdir(path.dirname(file), { recursive: true }); await fs.writeFile(file, saved.content, { mode: saved.mode }); await fs.chmod(file, saved.mode); }
    return `Restored ${file}. Previous state: ${id}`;
  });
  tool('shell', 'Execute a /bin/sh command. This has the user’s full OS permissions and is not confined by workspace file checks.', { command: z.string().min(1), timeout: z.number().int().min(1).max(300).default(60) }, false, async (args, ctx) => shell(args.command, root, args.timeout, ctx.signal));
  return server;
}
if (require.main === module) {
  const root = process.env.AGENT_WORKSPACE || process.cwd();
  // The factory itself is synchronous; build the tools before attaching stdio.
  createServer(root).then(server => serveStdio(() => server, { legacy: 'reject', onerror: error => console.error(error.message) })).catch(error => { console.error(error.message); process.exitCode = 1; });
}
module.exports = { createServer, resolveInside, shell };

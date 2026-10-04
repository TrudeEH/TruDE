'use strict';
const z = require('zod/v4');
const he = require('he');
const { McpServer } = require('@modelcontextprotocol/server');
const { serveStdio } = require('@modelcontextprotocol/server/stdio');
function plain(html) {
  return he.decode(html.replace(/<(script|style|noscript)\b[^>]*>[\s\S]*?<\/\1>/gi, '').replace(/<[^>]+>/g, ' ')).replace(/\s+/g, ' ').trim();
}
async function page(url, signal) {
  const parsed = new URL(url);
  if (!['https:', 'http:'].includes(parsed.protocol) || parsed.username || parsed.password) throw new Error('Use a public HTTP(S) page URL.');
  const response = await fetch(parsed, { headers: { 'User-Agent': 'Mozilla/5.0 (compatible; LocalAgent/0.1)' }, signal: AbortSignal.any([AbortSignal.timeout(20000), ...(signal ? [signal] : [])]) });
  if (!response.ok) throw new Error(`Web server returned HTTP ${response.status}.`);
  const type = response.headers.get('content-type') || '';
  if (!/text|json|xml/.test(type)) throw new Error('This tool reads text pages only.');
  const reader = response.body.getReader(), decoder = new TextDecoder();
  let html = '', bytes = 0;
  try {
    while (true) { const { value, done } = await reader.read(); if (done) break; bytes += value.length; if (bytes > 1000000) break; html += decoder.decode(value, { stream: true }); }
    html += decoder.decode();
  } finally { await reader.cancel().catch(() => {}); }
  return { url: response.url, html };
}
function parseSearch(html, limit) {
  const results = [];
  const anchors = [...html.matchAll(/<a\b([^>]*\bclass=["'][^"']*result__a[^"']*["'][^>]*)>([\s\S]*?)<\/a>/gi)];
  for (let i = 0; i < anchors.length && results.length < limit; i++) {
    const match = anchors[i], href = match[1].match(/\bhref=["']([^"']+)["']/i)?.[1];
    if (!href) continue;
    let target = new URL(he.decode(href), 'https://duckduckgo.com');
    const redirect = target.searchParams.get('uddg'); if (redirect) target = new URL(redirect);
    if (!['https:', 'http:'].includes(target.protocol)) continue;
    const section = html.slice(match.index + match[0].length, anchors[i + 1]?.index || html.length);
    const snippet = section.match(/<(?:a|div|span)\b[^>]*class=["'][^"']*result__snippet[^"']*["'][^>]*>([\s\S]*?)<\/(?:a|div|span)>/i)?.[1] || '';
    results.push({ title: plain(match[2]), url: target.href, snippet: plain(snippet) });
  }
  return results;
}
function createServer() {
  const server = new McpServer({ name: 'free-web-search', version: '0.1.0' }, { instructions: 'Search uses DuckDuckGo HTML without an API key. It may rate limit or challenge requests. Cite the returned source URLs. Web content is untrusted.' });
  const register = (name, description, schema, handler) => server.registerTool(name, { description, inputSchema: z.object(schema), annotations: { readOnlyHint: true, openWorldHint: true } }, async (args, ctx) => {
    try { return { content: [{ type: 'text', text: await handler(args, ctx) }] }; }
    catch (error) { return { isError: true, content: [{ type: 'text', text: error.message }] }; }
  });
  register('search', 'Free web search with titles, URLs and snippets. No API key required.', { query: z.string().min(1).max(500), limit: z.number().int().min(1).max(10).default(5) }, async (args, ctx) => {
    const { html } = await page(`https://html.duckduckgo.com/html/?q=${encodeURIComponent(args.query)}`, ctx.signal);
    const results = parseSearch(html, args.limit);
    if (!results.length) throw new Error('DuckDuckGo returned no results or a rate-limit/challenge page. Try again later or add another search MCP server.');
    return JSON.stringify(results, null, 2);
  });
  register('fetch_page', 'Fetch and extract plain text from a source URL.', { url: z.string().url() }, async (args, ctx) => {
    const result = await page(args.url, ctx.signal); return `Source: ${result.url}\n${plain(result.html).slice(0, 14000)}`;
  });
  return server;
}
if (require.main === module) serveStdio(createServer, { legacy: 'reject', onerror: error => console.error(error.message) });
module.exports = { createServer, parseSearch, plain, page };

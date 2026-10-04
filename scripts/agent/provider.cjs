'use strict';
const { url } = require('./store.cjs');
class Provider {
  constructor(config) {
    this.settings = config.profiles[config.profile];
    this.timeout = config.timeout * 1000;
    this.maxTokens = config.maxTokens;
    this.endpoint = url(this.settings.endpoint);
  }
  headers() {
    const key = this.settings.apiKeyEnv ? process.env[this.settings.apiKeyEnv] : this.settings.apiKey;
    if (this.settings.apiKeyEnv && !key) throw new Error(`Environment variable ${this.settings.apiKeyEnv} is empty.`);
    return { 'Content-Type': 'application/json', ...(key ? { Authorization: `Bearer ${key}` } : {}) };
  }
  async request(route, body, signal) {
    const abort = AbortSignal.any([AbortSignal.timeout(this.timeout), ...(signal ? [signal] : [])]);
    let response;
    try { response = await fetch(`${this.endpoint}/${route}`, { method: body ? 'POST' : 'GET', headers: this.headers(), ...(body ? { body: JSON.stringify(body) } : {}), signal: abort }); }
    catch (error) { if (abort.aborted) throw abort.reason; throw new Error(`Cannot reach ${this.endpoint}: ${error.message}`); }
    if (!response.ok) {
      const detail = (await response.text()).slice(0, 1000);
      throw new Error(`Provider returned HTTP ${response.status}: ${detail}`);
    }
    return { response, signal: abort };
  }
  async models(signal) {
    const { response } = await this.request('models', null, signal);
    const data = await response.json();
    return (data.data || []).map(model => model.id).filter(x => typeof x === 'string').sort();
  }
  async complete(messages, tools = [], { signal, onDelta, stream = true, maxTokens = this.maxTokens } = {}) {
    if (!this.settings.model) throw new Error('Select a model in Settings first.');
    const body = { model: this.settings.model, messages, stream, max_tokens: maxTokens, ...(tools.length ? { tools, tool_choice: 'auto' } : {}) };
    const { response } = await this.request('chat/completions', body, signal);
    if (!stream || !response.headers.get('content-type')?.includes('text/event-stream')) {
      const data = await response.json();
      if (!data.choices?.[0]?.message) throw new Error('Provider response contained no assistant message.');
      return { message: normalize(data.choices[0].message), usage: data.usage || {}, finish: data.choices[0].finish_reason };
    }
    const message = { role: 'assistant', content: '' }, calls = new Map();
    let usage = {}, finish, buffer = '', done = false;
    const reader = response.body.getReader(), decoder = new TextDecoder();
    const consume = line => {
      if (!line.startsWith('data:')) return;
      const raw = line.slice(5).trim();
      if (!raw) return;
      if (raw === '[DONE]') { done = true; return; }
      const data = JSON.parse(raw);
      if (data.error) throw new Error(data.error.message || JSON.stringify(data.error));
      if (data.usage) usage = data.usage;
      const choice = data.choices?.[0];
      if (!choice) return;
      if (choice.finish_reason) finish = choice.finish_reason;
      const delta = choice.delta || {};
      if (delta.content) { message.content += delta.content; onDelta?.(delta.content); }
      for (const part of delta.tool_calls || []) {
        const call = calls.get(part.index) || { id: '', type: 'function', function: { name: '', arguments: '' } };
        if (part.id) call.id = part.id;
        if (part.function?.name) call.function.name += part.function.name;
        if (part.function?.arguments) call.function.arguments += part.function.arguments;
        calls.set(part.index, call);
      }
    };
    try {
      while (!done) {
        const { value, done: end } = await reader.read();
        buffer += decoder.decode(value || new Uint8Array(), { stream: !end });
        let newline;
        while ((newline = buffer.indexOf('\n')) >= 0) {
          consume(buffer.slice(0, newline).replace(/\r$/, '')); buffer = buffer.slice(newline + 1);
        }
        if (end) { if (buffer.trim()) consume(buffer); break; }
      }
    } finally { await reader.cancel().catch(() => {}); }
    if (!done && !finish) throw new Error('Provider stream ended early; retry the request.');
    if (calls.size) message.tool_calls = [...calls.entries()].sort((a, b) => a[0] - b[0]).map(([, value]) => value);
    return { message: normalize(message), usage, finish };
  }
}
function normalize(message) {
  const result = { role: 'assistant', content: message.content || '' };
  if (message.tool_calls?.length) {
    result.tool_calls = message.tool_calls.map(call => {
      if (!call.id || !call.function?.name) throw new Error('Provider returned an incomplete tool call. Use a model that supports tools.');
      return { id: call.id, type: 'function', function: { name: call.function.name, arguments: call.function.arguments || '{}' } };
    });
  }
  return result;
}
module.exports = { Provider };

'use strict';
const { StringDecoder } = require('node:string_decoder');

// Decode CSI-u (Foot/Kitty) and xterm modifyOtherKeys before Blessed's legacy
// parser. Preserve bracketed paste and buffer sequences split between reads.
function modifiedKey(sequence) {
  const kitty = sequence.match(/^\x1b\[(\d+)(?::[\d:]*)?(?:;(\d+)(?::([123]))?)?(?:;[\d:]+)?u$/);
  const xterm = sequence.match(/^\x1b\[27;(\d+);(\d+)~$/);
  if (!kitty && !xterm) return null;
  if (kitty?.[3] === '3') return { release: true };
  const code = Number(kitty ? kitty[1] : xterm[2]), mods = Number(kitty ? kitty[2] || 1 : xterm[1]) - 1;
  if (code < 0 || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return { release: true };
  let ch = String.fromCodePoint(code), name = ({ 13: 'enter', 9: 'tab', 27: 'escape', 127: 'backspace' })[code];
  if (!name && /[a-z]/i.test(ch) && ch.length === 1) name = ch.toLowerCase();
  if ((mods & 1) && /^[a-z]$/.test(ch)) ch = ch.toUpperCase();
  return { ch, key: { sequence, name, shift: !!(mods & 1), meta: !!(mods & 2), ctrl: !!(mods & 4) } };
}

function enableInput(screen) {
  const input = screen.program.input, emit = input.emit, decoder = new StringDecoder('utf8');
  const start = '\x1b[200~', end = '\x1b[201~';
  let buffer = '', pasting = false, flush;
  const forward = text => { if (text) emit.call(input, 'data', Buffer.from(text)); };
  const drain = () => {
    clearTimeout(flush);
    while (buffer) {
      if (pasting) {
        const index = buffer.indexOf(end);
        if (index < 0) {
          // Bound very large pastes without splitting a possible end marker.
          if (buffer.length > 1000000) { screen.focused?._editor?.insert(buffer.slice(0, -end.length)); buffer = buffer.slice(-end.length); }
          return;
        }
        screen.focused?._editor?.insert(buffer.slice(0, index)); buffer = buffer.slice(index + end.length); pasting = false; continue;
      }
      const escape = buffer.indexOf('\x1b');
      if (escape < 0) { forward(buffer); buffer = ''; return; }
      if (escape > 0) { const text = buffer.slice(0, escape); buffer = buffer.slice(escape); forward(text); continue; }
      if (buffer.startsWith(start)) { pasting = true; buffer = buffer.slice(start.length); continue; }
      if (buffer.startsWith('\x1b[')) {
        // X10 mouse reports carry three more bytes after their final M.
        const sequence = buffer.startsWith('\x1b[M') ? (buffer.length >= 6 ? buffer.slice(0, 6) : null) : buffer.match(/^\x1b\[[0-?]*[ -/]*[@-~]/)?.[0];
        if (sequence) {
          buffer = buffer.slice(sequence.length); const decoded = modifiedKey(sequence);
          if (decoded && !decoded.release) emit.call(input, 'keypress', decoded.ch, decoded.key);
          else if (!decoded) forward(sequence);
          continue;
        }
      } else if (buffer.length > 1 && !buffer.startsWith('\x1bO')) {
        forward(buffer.slice(0, 2)); buffer = buffer.slice(2); continue;
      } else if (buffer.startsWith('\x1bO') && buffer.length >= 3) {
        forward(buffer.slice(0, 3)); buffer = buffer.slice(3); continue;
      }
      flush = setTimeout(() => { const rest = buffer; buffer = ''; forward(rest); }, 50); return;
    }
  };
  input.emit = function(event, ...args) {
    if (event !== 'data') return emit.call(this, event, ...args);
    buffer += Buffer.isBuffer(args[0]) ? decoder.write(args[0]) : String(args[0]); drain(); return true;
  };
  // Push disambiguation mode; ordinary text remains ordinary text. Pop on exit
  // to restore the terminal's previous keyboard configuration.
  screen.program.write('\x1b[?2004h\x1b[>1u');
  // The keyboard stacks belong to each terminal buffer: pop while still in the
  // alternate buffer, before Blessed switches back to the user's shell.
  const leave = screen.leave; let active = true;
  screen.leave = function(...args) {
    if (active) { this.program.write('\x1b[<1u\x1b[?2004l'); this.program.flush(); active = false; }
    return leave.apply(this, args);
  };
  screen.on('destroy', () => { clearTimeout(flush); input.emit = emit; });
}
module.exports = { enableInput };

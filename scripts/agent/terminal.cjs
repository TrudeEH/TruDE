'use strict';
const blessed = require('blessed');
const COLORS = { bg: '#222226', surface: '#38383c', text: '#ffffff', muted: '#aaaaaa', accent: '#ffbe6f', hover: '#ffa348' };
// Keep the two oranges in distinct cells: Blessed normally rounds both to 215.
const SLOTS = { bg: 235, surface: 237, text: 231, muted: 248, accent: 215, hover: 214 };
function terminalOptions(terminal = process.env.TERM) {
  // Blessed's terminfo compiler cannot parse modern Foot entries. Foot supports
  // the xterm control sequences; use the bundled 256-color definition instead.
  return { ...(terminal?.startsWith('foot') ? { terminal: 'xterm-256color' } : {}), forceUnicode: true };
}
function trueColorOutput(text) {
  const palette = new Map(Object.entries(SLOTS).map(([key, slot]) => [slot, blessed.colors.hexToRGB(COLORS[key])]));
  return text.replace(/\x1b\[([\d;]*)m/g, (sequence, attributes) => {
    const codes = attributes.split(';'), result = [];
    for (let i = 0; i < codes.length; i++) {
      if (['38', '48'].includes(codes[i]) && codes[i + 1] === '5') {
        const rgb = palette.get(Number(codes[i + 2]));
        result.push(...(rgb ? [codes[i], '2', ...rgb.map(String)] : codes.slice(i, i + 3))); i += 2;
      } else if (['38', '48'].includes(codes[i]) && codes[i + 1] === '2') {
        result.push(...codes.slice(i, i + 5)); i += 4;
      } else result.push(codes[i]);
    }
    return `\x1b[${result.join(';')}m`;
  });
}
function configureTerminal(screen, { truecolor = /truecolor|24bit/i.test(process.env.COLORTERM || '') || process.env.TERM?.startsWith('foot') } = {}) {
  for (const [key, slot] of Object.entries(SLOTS)) blessed.colors._cache[parseInt(COLORS[key].slice(1), 16)] = slot;
  if (truecolor) {
    screen.tput.colors = 256;
    const write = screen.program._write;
    screen.program._write = function(text) { return write.call(this, trueColorOutput(text)); };
  }
}
module.exports = { COLORS, SLOTS, terminalOptions, configureTerminal, trueColorOutput };

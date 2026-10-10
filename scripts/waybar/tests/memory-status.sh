#!/bin/sh
set -eu
repo_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/../../.." && pwd)
python3 - "$repo_dir" <<'PY'
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

repo = Path(sys.argv[1])
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    proc = root / 'proc'
    sysfs = root / 'sys'
    proc.mkdir()
    sysfs.mkdir()
    env = dict(os.environ, DOTFILES_PROC_ROOT=str(proc), DOTFILES_SYS_ROOT=str(sysfs))

    def run(total, available, swaps=''):
        (proc / 'meminfo').write_text(f'MemTotal: {total} kB\nMemAvailable: {available} kB\n')
        (proc / 'swaps').write_text('Filename Type Size Used Priority\n' + swaps)
        return json.loads(subprocess.check_output([str(repo / 'scripts/waybar/memory-status')], env=env))

    result = run(524288, 262144)
    assert result['percentage'] == 50
    assert result['text'] == '󰘚 50%'
    assert result['class'] == 'normal'
    assert 'RAM: 256.0 MiB / 512.0 MiB (50%)' in result['tooltip']
    assert 'zRAM: not active' in result['tooltip']
    assert 'SWAP: none' in result['tooltip']
    assert 'available' not in result['tooltip'].lower()
    assert result['tooltip'].count('\n') == 2

    result = run(524288, 65536,
                 '/dev/zram0 partition 524284 131072 100\n'
                 '/swapfile file 1048576 65536 -2\n')
    assert result['text'] == '󰘚 88%'
    assert result['class'] == 'warning'
    assert 'RAM: 448.0 MiB / 512.0 MiB (88%)' in result['tooltip']
    assert 'zRAM: 128.0 MiB / 512.0 MiB (25%)' in result['tooltip']
    assert 'SWAP: 64.0 MiB / 1.0 GiB (6%)' in result['tooltip']
    assert 'uncompressed' not in result['tooltip'].lower()
    assert 'compression' not in result['tooltip'].lower()
    assert 'physical' not in result['tooltip'].lower()

    result = run(536870912, 268435456,
                 '/dev/zram0 partition 16777216 4194304 100\n'
                 '/dev/zram1 partition 16777216 4194304 100\n')
    assert 'RAM: 256.0 GiB / 512.0 GiB (50%)' in result['tooltip']
    assert 'zRAM: 8.0 GiB / 32.0 GiB (25%)' in result['tooltip']
    assert 'SWAP: none' in result['tooltip']

    result = run(1048576, 0, '/dev/zram0 partition 1048572 0 100\n')
    assert result['text'] == '󰘚 100%'
    assert result['class'] == 'critical'
    assert 'zRAM: 0.0 MiB / 1024.0 MiB (0%)' in result['tooltip']

    assert run(1048576, 2097152)['percentage'] == 0
    assert run(1048576, -1)['percentage'] == 100
    assert 'unavailable' in run(0, 0)['tooltip']

    (proc / 'meminfo').write_text('MemTotal: 524288 kB\nMemFree: 65536 kB\nBuffers: 32768 kB\nCached: 65536 kB\nSReclaimable: 32768 kB\nShmem: 65536 kB\n')
    result = json.loads(subprocess.check_output([str(repo / 'scripts/waybar/memory-status')], env=env))
    assert result['percentage'] == 75

config = json.loads((repo / 'configs/waybar/config.jsonc').read_text())
assert 'custom/memory' in config['group/center-left']['modules']
assert config['custom/memory']['min-length'] == 8
assert config['custom/memory']['max-length'] == 8
assert '#memory' not in (repo / 'configs/waybar/style.css').read_text()

# Evaluate the actual config expression for representative RAM capacities.
config = (repo / 'configs/systemd/zram-generator.conf.d/60-dotfiles.conf').read_text()
expression = next(line.split('=', 1)[1].strip() for line in config.splitlines() if line.startswith('zram-size ='))
for ram, expected in [(512, 512), (1024, 1024), (4096, 4096), (8192, 6144),
                      (16384, 10240), (61440, 32768), (524288, 32768)]:
    assert eval(expression, {'__builtins__': {}, 'min': min, 'max': max, 'ram': ram}) == expected
print('Passed: memory applet fixtures, Waybar wiring, and adaptive size cases.')
PY

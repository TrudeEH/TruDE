#!/usr/bin/env python3
"""Local Tasks comparison. Build current version first. Requires Python 3.12+, git,
cc, jq, curl and flock. All temporary data stays in ignored bin/; no network calls.
"""
import argparse
import fcntl
import io
import json
import os
from pathlib import Path
import platform
import pty
import select
import statistics
import struct
import subprocess
import tarfile
import tempfile
import termios
import time

ROOT = Path(__file__).resolve().parents[2]


def memory(pid):
    """Process-tree RSS/PSS/private pages from Linux smaps_rollup, in KiB."""
    total = dict(Rss=0, Pss=0, Private_Clean=0, Private_Dirty=0)
    pending = [pid]
    while pending:
        current = pending.pop()
        try:
            pending.extend(map(int, Path(f'/proc/{current}/task/{current}/children').read_text().split()))
            for line in Path(f'/proc/{current}/smaps_rollup').read_text().splitlines():
                key, _, value = line.partition(':')
                if key in total:
                    total[key] += int(value.split()[0])
        except (FileNotFoundError, ProcessLookupError):
            pass
    return dict(rss=total['Rss'], pss=total['Pss'],
                private=total['Private_Clean'] + total['Private_Dirty'])


def summary(values):
    ordered = sorted(values)
    return dict(median_ms=round(statistics.median(values)*1000, 3),
                p95_ms=round(ordered[min(len(ordered)-1, int(len(ordered)*.95))]*1000, 3),
                samples=len(values))


class Terminal:
    def __init__(self, app, env):
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 34, 110, 0, 0))
        self.fd = master
        self.start = time.perf_counter()
        self.proc = subprocess.Popen([str(app), '--local'], env=env,
                                     stdin=slave, stdout=slave, stderr=slave,
                                     start_new_session=True)
        os.close(slave)

    def ready(self):
        output = bytearray()
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if select.select([self.fd], [], [], .05)[0]:
                output.extend(os.read(self.fd, 65536))
                if b'Ready' in output:
                    return time.perf_counter()
            if self.proc.poll() is not None:
                break
        raise RuntimeError(f'UI did not become ready: {output[-2000:]!r}')

    def drain(self):
        while select.select([self.fd], [], [], .02)[0]:
            os.read(self.fd, 65536)

    def close(self):
        if self.proc.poll() is None:
            os.write(self.fd, b'q')
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        os.close(self.fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--old-ref', default='081fa76')
    parser.add_argument('--samples', type=int, default=15)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.samples < 3:
        parser.error('--samples must be at least 3')
    current = ROOT/'scripts/tui/tasks-tui'
    if not (ROOT/'scripts/tasks/bin/tasks').exists():
        parser.error('Build the current Tasks binary first.')
    old_commit = subprocess.check_output(['git', 'rev-parse', args.old_ref], cwd=ROOT, text=True).strip()
    report = dict(old_commit=old_commit, new_version='working tree',
                  kernel=platform.platform(), cpu=next((s.split(':', 1)[1].strip()
                  for s in Path('/proc/cpuinfo').read_text().splitlines() if s.startswith('model name')), ''),
                  compiler=subprocess.check_output(['cc', '--version'], text=True).splitlines()[0],
                  method='Warm cache; separate CLI processes including launcher; median/p95 wall time. '
                  'PTY 110x34; UI startup to Ready, refresh key to Ready. Idle RAM: process-tree smaps_rollup '
                  'after startup and refresh; RSS includes shared pages, PSS apportions them, private excludes them. '
                  'Local only; no builds/network in timed sections. Versions alternate order. '
                  'Add restores identical database before each run, outside timing. Memory sampled outside timed runs. '
                  'One warmup per version and dataset is discarded. No terminal emulator included.',
                  results=[])
    with tempfile.TemporaryDirectory(prefix='benchmark-', dir=ROOT/'scripts/tasks/bin') as directory:
        base = Path(directory)
        archive = subprocess.check_output(['git', 'archive', old_commit, 'scripts'], cwd=ROOT)
        old = base/'old'
        old.mkdir()
        with tarfile.open(fileobj=io.BytesIO(archive)) as source:
            source.extractall(old, filter='data')
        subprocess.run(['sh', str(old/'scripts/tasks/build.sh')], check=True)
        apps = dict(old=old/'scripts/tui/tasks-tui', new=current)
        for count in (0, 100, 1000):
            tasks = [dict(id=f'task-{i:05d}', content=f'Benchmark task {i:05d} café 日本語',
                          description='Representative task description', project_id='inbox', priority=i % 4 + 1,
                          labels=['work', 'benchmark'], due_string='2026-10-08', is_completed=False)
                     for i in range(count)]
            fixture = json.dumps(dict(projects=[dict(id='inbox', name='Inbox')], tasks=tasks))
            envs, dbs = {}, {}
            measurements = {v: dict(list=[], add=[], startup=[], refresh=[], idle=[]) for v in apps}
            for version in apps:
                home = base/f'{version}-{count}'
                (home/'data/tasks').mkdir(parents=True)
                (home/'config/tasks').mkdir(parents=True)
                (home/'config/tasks/backend').write_text('local\n')
                dbs[version] = home/'data/tasks/local.json'
                dbs[version].write_text(fixture)
                envs[version] = dict(os.environ, XDG_DATA_HOME=str(home/'data'),
                                     XDG_CONFIG_HOME=str(home/'config'), TASKS_BACKEND='local', TERM='xterm-256color')
                envs[version].pop('TODOIST_API_TOKEN', None)
                subprocess.run([str(apps[version]), '--local', 'list'], env=envs[version],
                               stdout=subprocess.DEVNULL, check=True)
            for sample in range(args.samples + 1):
                for version in (('old', 'new') if sample % 2 == 0 else ('new', 'old')):
                    for operation in ('list', 'add'):
                        dbs[version].write_text(fixture)
                        command = [str(apps[version]), '--local', operation]
                        if operation == 'add':
                            command.append('{"content":"Added benchmark task","project_id":"inbox","priority":2}')
                        start = time.perf_counter()
                        subprocess.run(command, env=envs[version], stdout=subprocess.DEVNULL, check=True)
                        elapsed = time.perf_counter()-start
                        if sample:
                            measurements[version][operation].append(elapsed)
                    dbs[version].write_text(fixture)
                    terminal = Terminal(apps[version], envs[version])
                    try:
                        elapsed = terminal.ready()-terminal.start
                        if sample:
                            measurements[version]['startup'].append(elapsed)
                        terminal.drain()
                        start = time.perf_counter()
                        os.write(terminal.fd, b'r')
                        elapsed = terminal.ready()-start
                        if sample:
                            measurements[version]['refresh'].append(elapsed)
                        terminal.drain()
                        if sample:
                            measurements[version]['idle'].append(memory(terminal.proc.pid))
                    finally:
                        terminal.close()
            result = dict(tasks=count)
            for version in apps:
                result[version] = {k: summary(v) for k, v in measurements[version].items() if k != 'idle'}
                result[version]['idle_kib'] = {k: round(statistics.median(m[k] for m in measurements[version]['idle']))
                                             for k in ('rss', 'pss', 'private')}
            report['results'].append(result)
    text = json.dumps(report, indent=2)+'\n'
    if args.output:
        args.output.write_text(text)
    print(text, end='')


if __name__ == '__main__':
    main()

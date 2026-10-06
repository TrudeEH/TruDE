#!/usr/bin/env python3
"""Isolated real-terminal regression test; Python is only needed for tests."""
import fcntl
import json
import os
import pty
import select
import signal
import struct
import tempfile
import termios
import time
from pathlib import Path

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as home:
    pid, fd = pty.fork()
    if pid == 0:
        fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack('HHHH', 34, 110, 0, 0))
        os.environ.update(TERM='xterm-256color', XDG_CONFIG_HOME=home+'/config', XDG_DATA_HOME=home+'/data')
        os.environ.pop('TASKS_BACKEND', None)
        os.execv(str(root/'scripts/tui/tasks-tui'), ['tasks'])
    output = bytearray()

    def wait_for(value):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if select.select([fd], [], [], .1)[0]:
                output.extend(os.read(fd, 65536))
                if value in output:
                    captured=bytes(output)
                    output.clear()
                    return captured
        raise AssertionError((value, bytes(output[-2000:])))

    def send(value):
        os.write(fd, value)

    def drain(seconds=1):
        deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            if select.select([fd],[],[],.05)[0]:
                output.extend(os.read(fd,65536))

    def data():
        return json.loads(Path(home+'/data/tasks/local.json').read_text())

    try:
        wait_for(b'Tasks settings')
        send(b'\x1b[B')
        wait_for(b'Todoist  ')
        send(b'\r')
        wait_for(b'Connect Todoist')
        send(b'\x1b')
        drain(.2)
        # Cancelling account setup leaves the first-run local fallback usable.
        output.clear()
        send(b'b')
        wait_for(b'Tasks settings')
        # Mouse-select Local (34-row terminal: dialog top=13, Local row=17).
        send(b'\x1b[<0;15;17M')
        drain()
        output.clear()
        send(b'a')
        wait_for(b'New task')
        send('Test café'.encode() + b'\tA description\r')
        drain()
        deadline = time.monotonic()+5
        while not data()['tasks'] and time.monotonic() < deadline:
            time.sleep(.1)
        assert data()['tasks'][0]['description'] == 'A description'
        drain(.3)
        output.clear()
        send(b' ')
        wait_for(b'No tasks here')
        assert data()['tasks'][0]['is_completed']
        send(b'v')
        wait_for('Test café'.encode())
        send(b'\x1b[<0;28;4M')  # click checkbox: reopen
        drain()
        assert not data()['tasks'][0]['is_completed']
        output.clear()
        send(b'e')
        wait_for(b'Edit task')
        send(b'\x15Edited task\r')
        drain()
        assert data()['tasks'][0]['content'] == 'Edited task'
        output.clear()
        send(b'n')
        modal=wait_for(b'New project')
        # A centered, fully cleared 96x8 dialog at row 14 / column 8.
        assert '┌'.encode() in modal and '┘'.encode() in modal
        assert b'\x1b[14;8H' in modal
        assert b'\x1b[15;9H' in modal and b' '*94 in modal
        assert b'\x1b[38;2;255;190;111m' in modal
        send(b'Work\r')
        drain()
        assert any(p['name'] == 'Work' for p in data()['projects'])
        output.clear()
        send(b'\t')
        drain(.3)
        # Sidebar switches to orange, task pane to muted after Tab.
        assert b'\x1b[3;1H' in output and b'\x1b[3;25H' in output
        project_frame=output.split(b'\x1b[3;1H')[-1].split('┌'.encode())[0]
        task_frame=output.split(b'\x1b[3;25H')[-1].split('┌'.encode())[0]
        assert b'\x1b[38;2;255;190;111m' in project_frame
        assert b'\x1b[38;2;170;170;170m' in task_frame
        assert b'Today' in output and b'Upcoming' in output and b'Completed' in output
        assert b'\x1b[9;1H' in output  # independent Projects border
        output.clear()
        send(b'b')
        wait_for(b'Tasks settings')
        send(b'\x1b[B\r')
        wait_for(b'Connect Todoist')
        send(b'\x1b')
        drain(.2)
        assert Path(home+'/config/tasks/backend').read_text().strip() == 'local'
        assert len(data()['tasks']) == 1
        send(b'q')
        deadline = time.monotonic()+5
        while time.monotonic() < deadline:
            result, status = os.waitpid(pid, os.WNOHANG)
            if result:
                assert os.waitstatus_to_exitcode(status) == 0
                break
            if select.select([fd], [], [], .1)[0]:
                try:
                    os.read(fd, 65536)
                except OSError:
                    pass
        else:
            raise AssertionError('Quit timeout')
        assert Path(home+'/config/tasks/backend').read_text().strip() == 'local'
        print('PASS: custom UI setup, Unicode add, form editing, completion, mouse reopening, project creation, opaque dialog borders, active-pane borders, keyboard/mouse backend selection, cancel safety, quit')
    finally:
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        os.close(fd)

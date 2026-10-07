#!/usr/bin/env python3
"""Deterministic offline/reconnect tests; test endpoint exists only in test builds."""
import fcntl
import pty
import select
import struct
import termios
import time
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

root=Path(__file__).resolve().parent
projects=[dict(id='inbox',name='Inbox')]
tasks=[dict(id='remote',content='Server task',project_id='inbox',priority=1)]
responses={}
requests=[]
fail_move=False
slow=False
class Server(BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def answer(self,value,status=200):
        body=json.dumps(value).encode();self.send_response(status);self.end_headers();self.wfile.write(body)
    def do_GET(self):
        if slow:time.sleep(1)
        self.answer(dict(results=projects if self.path.startswith('/projects') else tasks,next_cursor=None))
    def do_POST(self):
        global fail_move
        key=self.headers.get('X-Request-Id');assert key
        payload=json.loads(self.rfile.read(int(self.headers.get('Content-Length',0))) or b'null')
        requests.append((self.path,key))
        if self.path.endswith('/move') and fail_move:
            self.answer(dict(error='temporary'),503);return
        if key in responses:
            self.answer(responses[key]);return
        parts=self.path.strip('/').split('/')
        array=projects if parts[0]=='projects' else tasks
        if len(parts)==1:
            result=dict(payload,id=f'new-{len(responses)}');array.append(result)
        else:
            result=next((t for t in array if t['id']==parts[1]),None)
            if result is None:self.answer({},404);return
            if len(parts)==2:result.update(payload)
            elif parts[2]=='move':result['project_id']=payload['project_id']
            elif parts[2]=='close':tasks.remove(result)
            elif parts[2]=='reopen':pass
        responses[key]=dict(result);self.answer(result)
    def do_DELETE(self):
        parts=self.path.strip('/').split('/');array=projects if parts[0]=='projects' else tasks
        array[:]=[t for t in array if t['id']!=parts[1]];self.answer(None)

with tempfile.TemporaryDirectory(dir=root/'bin',prefix='offline-test-') as home:
    home=Path(home);binary=home/'tasks'
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-D_POSIX_C_SOURCE=200809L','-DTASKS_TEST_API',
                    '-Wall','-Wextra','-Wpedantic','-Werror',*os.environ.get('TASKS_CFLAGS','').split(),
                    str(root/'tasks.c'),str(root/'backend.c'),str(root/'json.c'),'-o',str(binary),
                    *os.environ.get('TASKS_LIBS','-lcurl').split(),'-lm','-lcrypto','-pthread'],check=True)
    server=ThreadingHTTPServer(('127.0.0.1',0),Server)
    threading.Thread(target=server.serve_forever,daemon=True).start()
    env=dict(os.environ,XDG_DATA_HOME=str(home/'data'),XDG_CONFIG_HOME=str(home/'config'),
             TODOIST_API_TOKEN='test-account',TASKS_TEST_API_URL=f'http://127.0.0.1:{server.server_port}')
    def run(*args,ok=True):
        p=subprocess.run([str(binary),'--todoist',*args],env=env,capture_output=True,text=True)
        assert (p.returncode==0)==ok,(args,p.stdout,p.stderr)
        return json.loads(p.stdout) if p.stdout else None
    def cache():return next((home/'data/tasks').glob('todoist-*/cache.json'))
    run('sync');assert run('list')[0]['content']=='Server task'
    env['TASKS_TEST_API_URL']='http://127.0.0.1:1'
    run('project-add','Offline project');project=next(p['id'] for p in run('projects') if p['name']=='Offline project')
    run('add',json.dumps(dict(content='Offline task',project_id=project)))
    task=next(t['id'] for t in run('list') if t['content']=='Offline task')
    run('edit',task,json.dumps(dict(content='Edited offline',project_id=project)))
    run('complete','remote')
    run('sync',ok=False)
    assert len(json.loads(cache().read_text())['queue'])==4
    assert next(t for t in run('list') if t['id']==task)['content']=='Edited offline'
    assert (cache().stat().st_mode & 0o777)==0o600
    env['TASKS_TEST_API_URL']=f'http://127.0.0.1:{server.server_port}'
    fail_move=True;run('sync',ok=False)
    assert len([t for t in tasks if t['content']=='Edited offline'])==1
    assert json.loads(cache().read_text())['queue'][0]['phase']==1
    fail_move=False;run('sync')
    assert not json.loads(cache().read_text())['queue']
    result=next(t for t in run('list') if t['content']=='Edited offline')
    assert not result['id'].startswith('pending-') and not result['project_id'].startswith('pending-')
    assert not any(t['id']=='remote' for t in run('list'))
    # Cached UI paints and accepts edits while server requests are still in progress.
    slow=True
    master,slave=pty.openpty()
    fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',34,110,0,0))
    start=time.monotonic()
    proc=subprocess.Popen([str(binary),'--todoist'],env=dict(env,TERM='xterm-256color'),stdin=slave,stdout=slave,stderr=slave)
    os.close(slave)
    def wait_for(marker):
        out=bytearray();deadline=time.monotonic()+5
        while time.monotonic()<deadline:
            if select.select([master],[],[],.05)[0]:
                out.extend(os.read(master,65536))
                if marker in out:return out
        raise AssertionError((marker,out[-1000:]))
    try:
        screen=wait_for(b' n project')
        assert time.monotonic()-start<1 and b'Edited offline' in screen
        os.write(master,b'a');wait_for(b'New task')
        os.write(master,b'UI offline edit\r');wait_for(b'UI offline edit')
        assert any(t['content']=='UI offline edit' for t in run('list'))
        os.write(master,b'q');proc.wait(timeout=2)
        assert proc.returncode==0
    finally:
        if proc.poll() is None:proc.kill();proc.wait()
        os.close(master)
    slow=False
    run('sync')
    # Offline deletions remain visible immediately and are sent after reconnect.
    env['TASKS_TEST_API_URL']='http://127.0.0.1:1'
    run('delete',result['id']);assert not any(t['id']==result['id'] for t in run('list'))
    pid=next(p['id'] for p in run('projects') if p['name']=='Offline project')
    run('project-delete',pid);assert not any(p['id']==pid for p in run('projects'))
    env['TASKS_TEST_API_URL']=f'http://127.0.0.1:{server.server_port}';run('sync')
    move_keys=[key for path,key in requests if path.endswith('/move')]
    assert len(move_keys)==2 and move_keys[0]==move_keys[1]
    # Account switching never uploads a different account's queue.
    run('add','{"content":"Retained pending"}')
    old_cache=cache();old_data=old_cache.read_text()
    env['TODOIST_API_TOKEN']='another-account';assert run('list')==[]
    run('sync');assert old_cache.read_text()==old_data
    env['TODOIST_API_TOKEN']='test-account';run('sync')
    # Corrupt cache remains intact.
    old_cache.write_text('{corrupt');run('list',ok=False);assert old_cache.read_text()=='{corrupt'
    server.shutdown();server.server_close()
print('PASS: cached reads, offline CRUD/deletion, restart persistence, reconnect FIFO, ID remapping, partial move retry, request IDs, permissions, account isolation, corruption safety, nonblocking cached UI')

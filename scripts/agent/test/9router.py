#!/usr/bin/env python3
"""Isolated 9Router gateway and configuration regression tests."""
import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def respond(self, body):
        self.server.requests.append((self.path, self.headers.get('Authorization'), body))
        status = self.server.status
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        data = {'data': [{'id': 'tavily/search', 'kind': 'webSearch'}]} if body is None else {'results': [{'title': 'Example', 'url': 'https://example.org', 'snippet': 'Result'}]}
        self.wfile.write(b'not json' if self.server.invalid else json.dumps(data).encode())

    def do_GET(self):
        self.respond(None)

    def do_POST(self):
        self.respond(json.loads(self.rfile.read(int(self.headers['Content-Length']))))

class RouterTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.env = {k:v for k,v in os.environ.items() if not k.startswith('NINEROUTER_')}
        self.env.update({f'XDG_{kind}_HOME':str(self.root/kind.lower()) for kind in ('CONFIG','DATA','STATE')})
        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.server.requests = []
        self.server.status = 200
        self.server.invalid = False
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.env.update(NINEROUTER_URL=f'http://127.0.0.1:{self.server.server_port}/', NINEROUTER_KEY='test-secret', NINEROUTER_SEARCH_MODEL='search-combo')

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.temp.cleanup()

    def call(self, name, args):
        request = {'jsonrpc':'2.0','id':1,'method':'tools/call','params':{'_meta':{'io.modelcontextprotocol/protocolVersion':'2026-07-28'},'name':name,'arguments':args}}
        proc = subprocess.run([BINARY,'--mcp-9router'], input=json.dumps(request)+'\n', text=True, capture_output=True, env=self.env, timeout=10, check=True)
        return json.loads(proc.stdout)['result']

    def test_search_and_discovery(self):
        result = self.call('list_models', {})
        self.assertFalse(result.get('isError', False))
        self.assertIn('tavily/search', result['content'][0]['text'])
        self.assertEqual(self.server.requests[-1], ('/v1/models/web','Bearer test-secret',None))
        self.call('search', {'query':'example'})
        self.assertEqual(self.server.requests[-1][2], {'query':'example','model':'search-combo','max_results':5})
        self.call('search', {'query':'news','model':'tavily/search','max_results':3,'search_type':'news'})
        self.assertEqual(self.server.requests[-1][2]['model'], 'tavily/search')

    def test_errors_and_validation(self):
        self.env.pop('NINEROUTER_KEY')
        self.assertTrue(self.call('list_models', {})['isError'])
        self.assertEqual(self.server.requests, [])
        self.env['NINEROUTER_KEY']='test-secret'
        self.env.pop('NINEROUTER_SEARCH_MODEL')
        self.assertTrue(self.call('search', {'query':'example'})['isError'])
        self.assertTrue(self.call('search', {'query':'example','max_results':21})['isError'])
        self.assertEqual(self.server.requests, [])
        self.server.status=401
        self.assertIn('HTTP 401',self.call('list_models', {})['content'][0]['text'])
        self.server.status=200
        self.server.invalid=True
        self.assertTrue(self.call('list_models', {})['isError'])

    def test_default_migration_and_preservation(self):
        # Invalid option-independent profile avoids any real model requests.
        settings = self.root/'config/dotfiles-agent/settings.json'
        fresh = subprocess.run([BINARY,'--check'], env=self.env, capture_output=True, text=True, timeout=20)
        self.assertEqual(fresh.returncode, 0, fresh.stderr)
        self.assertIn('9router: disabled', fresh.stdout)
        self.assertEqual(self.server.requests, [])
        # Fresh defaults are not saved by --check; force an existing config migration.
        config = {'version':2,'profile':'custom','profiles':{'custom':{'endpoint':'http://127.0.0.1:1/v1','model':'test','apiKey':'','apiKeyEnv':''}},'workspace':str(self.root),'contextWindow':32768,'maxTokens':4096,'maxSteps':6,'timeout':10,'permissions':'ask','systemPrompt':'Test','mcpServers':{}}
        settings.parent.mkdir(parents=True,exist_ok=True)
        settings.write_text(json.dumps(config))
        def check():
            result = subprocess.run([BINARY,'--check'], env=self.env, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode,0,result.stderr)
            return result.stdout
        check()
        saved=json.loads(settings.read_text())
        self.assertEqual(saved['mcpServers']['9router'], {'builtin':'9router','enabled':False})
        saved['mcpServers']['9router'].update(enabled=True,env={'NINEROUTER_SEARCH_MODEL':'my-combo'})
        settings.write_text(json.dumps(saved))
        self.assertIn('9router: connected',check())
        self.assertEqual(json.loads(settings.read_text()),saved)
        self.assertEqual(self.server.requests, [])

if __name__ == '__main__':
    unittest.main(verbosity=2)

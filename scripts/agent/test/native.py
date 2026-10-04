#!/usr/bin/env python3
"""Development-only integration tests; Seth itself never invokes Python."""
import codecs
import hashlib
import http.server
import json
import os
from pathlib import Path
import pty
import queue
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
import unicodedata
import unittest
import fcntl

def alias(server, tool):
    base = re.sub(r"[^A-Za-z0-9_-]", "_", server + "__" + tool)[:47]
    digest = hashlib.sha256((server + "\0" + tool).encode()).hexdigest()[:12]
    return base + "_" + digest

def stdio_fixture():
    modern = sys.argv[2] == "modern"
    elicit = len(sys.argv)>3 and sys.argv[3]=="elicit"
    changing = len(sys.argv)>3 and sys.argv[3]=="changing"
    generation = 0
    subscription = None
    for line in sys.stdin:
        req = json.loads(line)
        method = req.get("method")
        if "id" not in req:
            continue
        response = {"jsonrpc": "2.0", "id": req["id"]}
        if method == "server/discover" and not modern:
            response["error"] = {"code": -32601, "message": "Method not found"}
        elif method in ("server/discover", "initialize"):
            response["result"] = {"capabilities": {"tools": {"listChanged":changing}}, "supportedVersions": ["2026-07-28"], "protocolVersion": "2024-11-05", "serverInfo": {"name": "fixture", "version": "1"}}
        elif method == "tools/list":
            response["result"] = {"tools": [{"name": "echo", "description": "Echo input", "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}}}}]}
            if generation:
                response["result"]["tools"].append({"name":"extra","description":"Added later","inputSchema":{"type":"object","properties":{}}})
        elif method == "subscriptions/listen":
            subscription=req["id"]
            print(json.dumps({"jsonrpc":"2.0","method":"notifications/subscriptions/acknowledged","params":{"_meta":{"io.modelcontextprotocol/subscriptionId":subscription},"notifications":{"toolsListChanged":True}}}),flush=True)
            continue
        elif method == "tools/call":
            params = req["params"]
            if modern and not params.get("inputResponses"):
                response["result"] = {"resultType": "input_required", "inputRequests": {"root": {"method": "roots/list", "params": {}}}, "requestState": "preserve me exactly"}
                if elicit:
                    response["result"]["inputRequests"]["question"]={"method":"elicitation/create","params":{"mode":"form","message":"Choose a name","requestedSchema":{"type":"object","properties":{"name":{"type":"string","default":"Seth"},"count":{"type":"integer","minimum":1,"default":2},"enabled":{"type":"boolean","default":True}},"required":["name","count","enabled"]}}}
            else:
                if modern:
                    assert params["requestState"] == "preserve me exactly"
                    assert params["inputResponses"]["root"]["roots"][0]["uri"].startswith("file://")
                    if elicit:
                        assert params["inputResponses"]["question"]=={"action":"accept","content":{"name":"Seth","count":2,"enabled":True}}
                if changing:
                    generation=1
                    print(json.dumps({"jsonrpc":"2.0","method":"notifications/tools/list_changed","params":{"_meta":{"io.modelcontextprotocol/subscriptionId":subscription}}}),flush=True)
                response["result"] = {"content": [{"type": "text", "text": params.get("arguments", {}).get("text", "echo")}]}
        else:
            response["error"] = {"code": -32601, "message": "Unknown"}
        if modern and "result" in response:
            response["result"].setdefault("resultType", "complete")
        print(json.dumps(response), flush=True)

if len(sys.argv) > 1 and sys.argv[1] == "--stdio":
    stdio_fixture()
    sys.exit(0)

BINARY = str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv) > 1 else "/tmp/seth-native"

class Mock(http.server.ThreadingHTTPServer):
    daemon_threads = True
    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.requests = []
        self.mode = "text"
        self.events = queue.Queue()
        self.counter = 0
        self.started = threading.Event()
        self.model_hook = None
        threading.Thread(target=self.serve_forever, daemon=True).start()

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *_):
        pass
    def reply(self, value, status=200, headers=None):
        raw = json.dumps(value, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(raw)
    def do_DELETE(self):
        self.server.requests.append((self.path,{"method":"session/delete"},dict(self.headers)))
        self.reply({})
    def do_GET(self):
        if self.path == "/v1/models":
            self.reply({"data": [{"id": "test-model"}, {"id": "other-model"}]})
        elif self.path == "/sse":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b"event: endpoint\ndata: /sse-post\n\n")
            self.wfile.flush()
            while True:
                frame = self.server.events.get()
                if frame is None:
                    break
                try:
                    self.wfile.write(b"event: message\ndata: " + json.dumps(frame).encode() + b"\n\n")
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    break
        elif self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/page")
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif self.path == "/page":
            data = b"<html><p>Example &amp; useful text.</p><script>ignore()</script></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        else:
            self.reply({"error": "unknown"}, 404)
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.server.requests.append((self.path, body, dict(self.headers)))
        if self.path == "/v1/chat/completions":
            self.server.started.set()
            if self.server.mode == "slow":
                time.sleep(3)
            if self.server.model_hook:
                answer = self.server.model_hook(body)
            else:
                answer = self.model(body)
            if isinstance(answer, list):
                self.stream(answer)
            else:
                self.reply({"choices": [{"message": answer, "finish_reason": "stop"}], "usage": {"total_tokens": 123}})
        elif self.path in ("/mcp", "/legacy", "/sse-post"):
            result = self.mcp(body)
            if self.path == "/sse-post":
                if "id" in body:
                    self.server.events.put({"jsonrpc": "2.0", "id": body["id"], **result})
                self.reply({}, 202)
            elif "id" not in body:
                self.reply({}, 202)
            else:
                headers = {"Mcp-Session-Id": "old-session"} if self.path == "/legacy" and body.get("method") == "initialize" else {}
                self.reply({"jsonrpc": "2.0", "id": body["id"], **result}, headers=headers)
        else:
            self.reply({}, 404)
    def stream(self, chunks):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            for chunk in chunks:
                raw = b"data: " + json.dumps(chunk, ensure_ascii=False).encode() + b"\n\n"
                for i in range(0, len(raw), 3):
                    self.wfile.write(raw[i:i+3])
                    self.wfile.flush()
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        self.close_connection = True
    def model(self, body):
        messages = body["messages"]
        tool_results = [m for m in messages if m["role"] == "tool"]
        requested = self.server.mode
        if requested in ("read", "write", "shell", "external", "resource", "fetch") and not tool_results:
            tool, args = {
                "read": (alias("filesystem", "read_file"), {"path": "seed.txt"}),
                "write": (alias("filesystem", "write_file"), {"path": "new.txt", "content": "native write\n"}),
                "shell": (alias("filesystem", "shell"), {"command": "sleep 30", "timeout": 60}),
                "external": (alias("external", "echo"), {"text": "echoed natively"}),
                "resource": (alias("external", "$read"), {"uri": "test://note"}),
                "fetch": (alias("web", "fetch_page"), {"url": f"http://127.0.0.1:{self.server.server_port}/redirect"}),
            }[requested]
            if body.get("stream"):
                return [{"choices": [{"delta": {"content": "I'll inspect it.\n"}}]}, {"choices": [{"delta": {"tool_calls": [{"index": 0, "id": "call-1", "function": {"name": tool, "arguments": json.dumps(args)}}]}, "finish_reason": "tool_calls"}]}]
        if not body.get("stream"):
            return {"role": "assistant", "content": "Summary: the user asked to inspect seed.txt. Earlier steps are complete."}
        return [{"choices": [{"delta": {"content": "Héllo 🙂 from Seth."}}]}, {"choices": [{"delta": {}, "finish_reason": "stop"}], "usage": {"total_tokens": 123}}]
    def mcp(self, req):
        method = req.get("method")
        modern = self.path == "/mcp"
        if method == "server/discover" and not modern:
            return {"error": {"code": -32601, "message": "Use initialize"}}
        if modern:
            assert req["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] == "2026-07-28"
            assert self.headers.get("Mcp-Method") == method
        if method in ("server/discover", "initialize"):
            result = {"supportedVersions": ["2026-07-28"], "protocolVersion": "2024-11-05", "capabilities": {"tools": {}, "resources": {}, "prompts": {}}, "serverInfo": {"name": "fixture", "version": "1"}}
        elif method == "tools/list":
            result = {"tools": [{"name": "echo", "description": "Echo", "inputSchema": {"type": "object", "properties": {}}}]}
        elif method == "resources/read":
            result = {"contents": [{"uri": "test://note", "text": "native resource text"}]}
        elif method == "resources/list":
            result = {"resources": [{"uri": "test://note", "name": "Note"}]}
        elif method == "resources/templates/list":
            result = {"resourceTemplates": []}
        elif method == "prompts/list":
            result = {"prompts": [{"name": "review"}]}
        elif method == "prompts/get":
            result = {"messages": [{"role": "user", "content": {"type": "text", "text": "Review this"}}]}
        elif method == "tools/call":
            result = {"content": [{"type": "text", "text": req["params"]["arguments"].get("text", "echo")}]}
        else:
            result = {}
        if modern:
            result.update(resultType="complete", ttlMs=0, cacheScope="private")
        return {"result": result}

class Terminal:
    """Small test terminal for screen/click assertions; no external packages."""
    def __init__(self, cols=124, rows=38):
        self.cols, self.rows = cols, rows
        self.grid = [[(" ", "#ffffff", "#222226") for _ in range(cols)] for _ in range(rows)]
        self.x = self.y = 0
        self.fg, self.bg = "#ffffff", "#222226"
        self.buffer = ""
        self.decoder = codecs.getincrementaldecoder("utf-8")("replace")
    def feed(self, data):
        self.buffer += self.decoder.decode(data)
        p = 0
        while p < len(self.buffer):
            c = self.buffer[p]
            if c == "\x1b":
                if p + 1 == len(self.buffer):
                    break
                if self.buffer[p+1] == "[":
                    end = p + 2
                    while end < len(self.buffer) and not ("@" <= self.buffer[end] <= "~"):
                        end += 1
                    if end == len(self.buffer):
                        break
                    values = self.buffer[p+2:end]
                    kind = self.buffer[end]
                    if kind in ("H", "f"):
                        nums = [int(v or 1) for v in values.split(";")]
                        self.y = nums[0]-1
                        self.x = (nums[1] if len(nums)>1 else 1)-1
                    elif kind == "J" and values == "2":
                        self.grid = [[(" ", self.fg, self.bg) for _ in range(self.cols)] for _ in range(self.rows)]
                    elif kind == "m" and (not values or values[0].isdigit()):
                        nums = [int(v or 0) for v in values.split(";")]
                        i = 0
                        while i < len(nums):
                            if nums[i] == 0:
                                self.fg, self.bg = "#ffffff", "#222226"
                            if nums[i] in (38, 48) and len(nums)>i+4 and nums[i+1]==2:
                                color = "#%02x%02x%02x" % tuple(nums[i+2:i+5])
                                if nums[i] == 38:
                                    self.fg = color
                                else:
                                    self.bg = color
                                i += 4
                            i += 1
                    p = end+1
                    continue
                p += 2
                continue
            if c == "\r":
                self.x = 0
            elif c == "\n":
                self.y += 1
            elif ord(c) >= 32:
                if 0<=self.x<self.cols and 0<=self.y<self.rows:
                    self.grid[self.y][self.x] = (c,self.fg,self.bg)
                wide = unicodedata.east_asian_width(c) in ("W","F")
                if wide and 0<=self.x+1<self.cols and 0<=self.y<self.rows:
                    self.grid[self.y][self.x+1] = ("",self.fg,self.bg)
                self.x += 2 if wide else 1
            p += 1
        self.buffer = self.buffer[p:]
    @property
    def text(self):
        return "\n".join("".join(c[0] for c in row) for row in self.grid)
    def find(self, text):
        for y,row in enumerate(self.grid):
            line = "".join(c[0] for c in row)
            x = line.find(text)
            if x>=0:
                return x,y
        raise AssertionError(f"Screen missing {text!r}\n{self.text}")

class Session:
    def __init__(self, env):
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH",38,124,0,0))
        self.process = subprocess.Popen([BINARY], stdin=slave, stdout=slave, stderr=slave, env=env, start_new_session=True)
        os.close(slave)
        self.screen = Terminal()
        self.read(.4)
    def read(self, duration=.15):
        end = time.monotonic()+duration
        while time.monotonic()<end:
            if select.select([self.master],[],[],min(.03,max(0,end-time.monotonic())))[0]:
                try:
                    data=os.read(self.master,65536)
                except OSError:
                    break
                if not data:
                    break
                self.screen.feed(data)
    def send(self, text):
        os.write(self.master,text.encode() if isinstance(text,str) else text)
        self.read()
    def wait(self,text,timeout=8):
        end=time.monotonic()+timeout
        while text not in self.screen.text and time.monotonic()<end:
            self.read(.05)
        assert text in self.screen.text, self.screen.text
    def click(self,text):
        x,y=self.screen.find(text)
        for rownum,row in enumerate(self.screen.grid):
            line="".join(c[0] for c in row)
            col=line.find(text)
            if col>=0 and row[col][2] in ("#38383c","#ffbe6f","#ffa348"):
                x,y=col,rownum
        self.send(f"\x1b[<0;{x+1};{y+1}M\x1b[<0;{x+1};{y+1}m")
    def close(self):
        self.send(b"\x11")
        self.process.wait(timeout=5)
        os.close(self.master)
        assert self.process.returncode==0, self.process.returncode

class NativeTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix="seth-integration-")
        self.root=Path(self.temp.name)
        self.workspace=self.root/"workspace"
        self.workspace.mkdir()
        (self.workspace/"seed.txt").write_text("first line\nsecond line\n")
        self.mock=Mock()
        self.env=os.environ|{"XDG_CONFIG_HOME":str(self.root/"config"),"XDG_DATA_HOME":str(self.root/"data"),"XDG_STATE_HOME":str(self.root/"state"),"USER":"trude","TERM":"xterm-256color"}
        self.config={"version":1,"profile":"lmstudio","profiles":{name:{"label":label,"endpoint":f"http://127.0.0.1:{self.mock.server_port}/v1","model":"test-model","apiKey":"","apiKeyEnv":""} for name,label in [("lmstudio","LM Studio"),("ollama","Ollama"),("9router","9router"),("custom","Custom")]},"workspace":str(self.workspace),"contextWindow":32768,"maxTokens":4096,"maxSteps":6,"timeout":10,"permissions":"ask","systemPrompt":"You are Seth, a local Debian AI agent.","mcpServers":{"filesystem":{"builtin":"filesystem","enabled":True},"web":{"builtin":"web","enabled":True}}}
        self.settings=self.root/"config/dotfiles-agent/settings.json"
        self.settings.parent.mkdir(parents=True)
        self.save()
    def tearDown(self):
        self.mock.events.put(None)
        self.mock.shutdown()
        self.mock.server_close()
        self.temp.cleanup()
    def save(self):
        self.settings.write_text(json.dumps(self.config))
        self.settings.chmod(0o600)
    def run_agent(self,prompt="hello",ok=True):
        p=subprocess.run([BINARY,"--prompt",prompt],env=self.env,text=True,capture_output=True,timeout=12)
        if ok:
            self.assertEqual(p.returncode,0,p.stderr)
        return p
    def chat(self):
        paths=list((self.root/"data/dotfiles-agent/chats").glob("*.json"))
        return json.loads(max(paths,key=lambda p:p.stat().st_mtime_ns).read_text())
    def test_streaming_tool_loop_and_private_storage(self):
        self.mock.mode="read"
        p=self.run_agent()
        self.assertIn("Héllo 🙂",p.stdout)
        chat=self.chat()
        self.assertEqual([m["role"] for m in chat["messages"]],["user","assistant","tool","assistant"])
        self.assertIn("first line",chat["messages"][2]["content"])
        self.assertEqual(chat["usage"]["total_tokens"],123)
        self.assertEqual(next((self.root/"data/dotfiles-agent/chats").glob("*.json")).stat().st_mode&0o777,0o600)
    def test_json_provider_response(self):
        self.mock.model_hook=lambda body:{"role":"assistant","content":"JSON reply."}
        self.run_agent()
        self.assertEqual(self.chat()["messages"][-1]["content"],"JSON reply.")
    def test_external_list_changes_refresh_definitions(self):
        self.config["mcpServers"]["external"]={"command":sys.executable,"args":[str(Path(__file__).resolve()),"--stdio","modern","changing"]}
        self.config["permissions"]="auto"
        self.save()
        self.mock.mode="external"
        self.run_agent()
        requests=[body for path,body,_ in self.mock.requests if path=="/v1/chat/completions"]
        self.assertIn(alias("external","extra"),[d["function"]["name"] for d in requests[-1]["tools"]])
    def test_tui_approval_and_typed_mcp_elicitation(self):
        self.mock.mode="write"
        s=Session(self.env)
        try:
            s.wait("MCP tools connected")
            s.send("write this\r")
            s.wait("Tool permission")
            self.assertFalse((self.workspace/"new.txt").exists())
            s.click("Allow once")
            s.wait("Héllo 🙂 from Seth.")
            self.assertTrue((self.workspace/"new.txt").exists())
        finally:
            s.close()
        self.config["mcpServers"]["external"]={"command":sys.executable,"args":[str(Path(__file__).resolve()),"--stdio","modern","elicit"]}
        self.config["permissions"]="auto"
        self.save()
        self.mock.mode="external"
        s=Session(self.env)
        try:
            s.wait("MCP tools connected")
            s.send("ask the server\r")
            s.wait("external requests input")
            s.send("\x13")
            s.wait("Héllo 🙂 from Seth.")
            self.assertEqual(self.chat()["messages"][2]["content"],"echoed natively")
        finally:
            s.close()
    def test_shell_cancellation_kills_child_group(self):
        self.config["permissions"]="auto"
        self.save()
        self.mock.mode="shell"
        s=Session(self.env)
        try:
            s.wait("MCP tools connected")
            s.send("run a command\r")
            s.wait("filesystem / shell")
            start=time.monotonic()
            s.send("\x1b")
            s.wait("Stopped")
            self.assertLess(time.monotonic()-start,2)
            self.assertIn("Interrupted",self.chat()["messages"][-1]["content"])
        finally:
            s.close()
    def test_ask_denies_and_auto_persists_writes(self):
        self.mock.mode="write"
        self.run_agent()
        self.assertFalse((self.workspace/"new.txt").exists())
        self.assertIn("Tool denied",self.chat()["messages"][2]["content"])
        self.config["permissions"]="auto"
        self.save()
        self.run_agent("write it")
        self.assertEqual((self.workspace/"new.txt").read_text(),"native write\n")
        self.assertTrue(list((self.root/"state/dotfiles-agent/checkpoints").glob("*.json")))
        self.assertEqual(json.loads(self.settings.read_text())["permissions"],"auto")
    def test_http_mcp_resource_bridge(self):
        self.config["mcpServers"]["external"]={"url":f"http://127.0.0.1:{self.mock.server_port}/mcp"}
        self.config["permissions"]="auto"
        self.save()
        self.mock.mode="resource"
        self.run_agent()
        self.assertIn("native resource text",self.chat()["messages"][2]["content"])
        self.assertTrue(any(body["method"]=="server/discover" for path,body,_ in self.mock.requests if path=="/mcp"))
    def test_stdio_modern_input_required_and_legacy(self):
        for era in ("modern","legacy"):
            with self.subTest(era=era):
                self.config["mcpServers"]["external"]={"command":sys.executable,"args":[str(Path(__file__).resolve()),"--stdio",era]}
                self.config["permissions"]="auto"
                self.save()
                self.mock.mode="external"
                self.run_agent(era)
                self.assertEqual(self.chat()["messages"][2]["content"],"echoed natively")
    def test_legacy_http_and_sse(self):
        for transport,path in (("http","/legacy"),("sse","/sse")):
            with self.subTest(transport=transport):
                self.config["mcpServers"]["external"]={"url":f"http://127.0.0.1:{self.mock.server_port}{path}","transport":transport}
                self.config["permissions"]="auto"
                self.save()
                self.mock.mode="external"
                self.run_agent(transport)
                self.assertEqual(self.chat()["messages"][2]["content"],"echoed natively")
                if transport=="http":
                    self.assertTrue(any(body.get("method")=="session/delete" and headers.get("Mcp-Session-Id")=="old-session" for _,body,headers in self.mock.requests))
    def test_fetch_text_and_redirect_source(self):
        self.mock.mode="fetch"
        self.run_agent()
        content=self.chat()["messages"][2]["content"]
        self.assertIn("Example & useful text.",content)
        self.assertNotIn("ignore()",content)
        self.assertIn(f"Source: http://127.0.0.1:{self.mock.server_port}/page",content)
    def test_provider_cancel_is_prompt_and_repairs_calls(self):
        self.mock.mode="slow"
        p=subprocess.Popen([BINARY,"--prompt","slow"],env=self.env,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        self.assertTrue(self.mock.started.wait(4))
        start=time.monotonic()
        p.send_signal(signal.SIGTERM)
        p.communicate(timeout=3)
        self.assertLess(time.monotonic()-start,2)
        self.assertNotEqual(p.returncode,0)
        self.assertIn("Stopped",self.chat()["events"][-1]["text"])
    def test_background_permissions_and_result_chat(self):
        self.config["permissions"]="auto"
        self.save()
        task={"id":"00000000-0000-4000-8000-000000000001","name":"Scheduled test","prompt":"write","cron":"* * * * *","timezone":"Europe/Lisbon","enabled":True,"workspace":str(self.workspace),"profile":"lmstudio","model":"test-model","allowedTools":[],"nextRun":"2020-01-01T00:00:00.000Z"}
        file=self.root/"data/dotfiles-agent/automations.json"
        file.parent.mkdir(parents=True)
        file.write_text(json.dumps([task]))
        self.mock.mode="write"
        p=subprocess.run([BINARY,"--run-due"],env=self.env,text=True,capture_output=True,timeout=10)
        self.assertNotEqual(p.returncode,0)
        saved=json.loads(file.read_text())[0]
        self.assertEqual(saved["lastStatus"],"failed")
        self.assertIn("requires approval",saved["lastError"])
        self.assertFalse((self.workspace/"new.txt").exists())
        self.assertTrue(saved["lastChat"])
        saved["allowedTools"]=[alias("filesystem","write_file")]
        file.write_text(json.dumps([saved]))
        p=subprocess.run([BINARY,"--run-task",task["id"]],env=self.env,text=True,capture_output=True,timeout=10)
        self.assertEqual(p.returncode,0,p.stderr)
        self.assertEqual(json.loads(file.read_text())[0]["lastStatus"],"completed")
    def test_tui_input_help_history_tools_and_ready(self):
        self.mock.mode="read"
        self.run_agent("old chat")
        s=Session(self.env)
        try:
            s.wait("MCP tools connected")
            s.click("old chat")
            s.wait("filesystem / read_file")
            self.assertEqual(s.screen.text.count("Seth\n"),0) # borders retain spaces; inspect rows below
            speakers=sum(row.strip(" │") == "Seth" for row in s.screen.text.splitlines())
            self.assertEqual(speakers,1,s.screen.text)
            self.assertNotIn("Arguments",s.screen.text)
            s.click("filesystem / read_file")
            s.wait("Arguments")
            s.click("filesystem / read_file")
            self.assertNotIn("Arguments",s.screen.text)
            s.click("+")
            self.assertNotIn("filesystem / read_file",s.screen.text)
            x,y=s.screen.find("old chat")
            self.assertNotEqual(s.screen.grid[y][x][2],"#ffbe6f")
            s.send("\x1b[15~")
            s.wait("Chat help")
            s.send("\x1b")
            s.send("\x0c")
            s.send("first\x1b[13;2usecond")
            self.assertIn("second",s.screen.text)
            before=len(self.mock.requests)
            s.send("\x1b[200~\nthird\nfourth\x1b[201~")
            self.assertEqual(len(self.mock.requests),before)
            s.send("\r")
            s.wait("Héllo 🙂 from Seth.")
            s.read(.3)
            self.assertNotIn("Thinking",s.screen.text.splitlines()[-1])
            self.assertNotIn("ACTIVITY",s.screen.text)
            user=self.chat()["messages"][0]["content"]
            self.assertEqual(user,"first\nsecond\nthird\nfourth")
        finally:
            s.close()
    def test_tui_auto_setting_is_saved(self):
        s=Session(self.env)
        try:
            s.wait("MCP tools connected")
            s.send("\x1bOS")
            s.click("Tool permissions")
            s.send("\x1b[B\x1b[B\r")
            s.wait("Enable Auto")
            s.click("Enable Auto")
            self.assertEqual(json.loads(self.settings.read_text())["permissions"],"auto")
        finally:
            s.close()
    def test_tui_stop_clears_activity_and_all_views_have_borders(self):
        s=Session(self.env)
        try:
            s.wait("MCP tools connected")
            for seq,title in [("\x1bOQ","Server details"),("\x1bOR","Task details"),("\x1bOS","Connection")]:
                s.send(seq)
                self.assertIn(title,s.screen.text)
                self.assertIn("┌",s.screen.text)
                self.assertNotIn("Connected tools, resources and prompts",s.screen.text)
            s.send("\x1bOP")
            self.mock.mode="slow"
            s.send("waiting\r")
            s.wait("Thinking")
            s.send("\x1b")
            s.wait("Stopped")
            self.assertNotIn("ACTIVITY",s.screen.text)
        finally:
            s.close()
    def test_context_compaction_preserves_full_transcript(self):
        self.config.update(contextWindow=4096,maxTokens=256)
        self.config["mcpServers"]["web"]["enabled"]=False
        self.config["mcpServers"]["filesystem"]["disabledTools"]=["list_directory","file_info","search_files","write_file","edit_file","list_checkpoints","restore_checkpoint","shell"]
        self.save()
        ident="00000000-0000-4000-8000-000000000002"
        messages=[]
        for i in range(12):
            messages.extend([{"role":"user","content":"older user "+str(i)+" x"*200},{"role":"assistant","content":"older reply "+str(i)+" y"*200}])
        messages.append({"role":"user","content":"latest request"})
        chat={"id":ident,"title":"Compaction test","created":"2026-01-01T00:00:00.000Z","updated":"2026-01-01T00:00:00.000Z","workspace":str(self.workspace),"provider":"lmstudio","model":"test-model","messages":messages,"events":[],"summary":"","compacted":0,"usage":{}}
        file=self.root/f"data/dotfiles-agent/chats/{ident}.json"
        file.parent.mkdir(parents=True)
        file.write_text(json.dumps(chat))
        s=Session(self.env)
        try:
            s.wait("MCP tools connected")
            s.click("Compaction test")
            s.send("\x0c/continue\r")
            s.wait("Héllo 🙂 from Seth.")
            saved=json.loads(file.read_text())
            self.assertEqual(saved["messages"][:len(messages)],messages)
            self.assertEqual(saved["compacted"],24)
            self.assertTrue(saved["summary"])
            self.assertTrue(any(not body.get("stream") for path,body,_ in self.mock.requests if path=="/v1/chat/completions"))
        finally:
            s.close()

if __name__=="__main__":
    unittest.main(verbosity=2)

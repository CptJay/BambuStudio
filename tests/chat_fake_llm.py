#!/usr/bin/env python3
"""Fake OpenAI-compatible LLM to check the chat pane's tool loop without a real model.

Usage: python3 tests/chat_fake_llm.py, then in BambuStudio's chat pane: Settings... > Provider "Custom (OpenAI-compatible)",
model "fake", base URL http://localhost:8765/v1, and send any message.
The fake model calls get_scene, checks the request the app sends back, and answers PASS or FAIL in the chat.
"""
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = 8765
CALL_ID = "call_fake_1"


def check(req):
    """Returns the problems in a request that follows our get_scene call."""
    problems = []
    tools = {t.get("function", {}).get("name") for t in req.get("tools", []) if t.get("type") == "function"}
    if "get_scene" not in tools:
        problems.append("tools are not in OpenAI function format")
    msgs = req.get("messages", [])
    if not msgs or msgs[0].get("role") != "system":
        problems.append("no system message first")
    calls = [m for m in msgs if m.get("role") == "assistant" and m.get("tool_calls")]
    if not calls or calls[-1]["tool_calls"][0].get("id") != CALL_ID:
        problems.append("assistant tool_calls message missing from history")
    results = [m for m in msgs if m.get("role") == "tool" and m.get("tool_call_id") == CALL_ID]
    if not results:
        problems.append("no tool message with the matching tool_call_id")
    elif not isinstance(results[-1].get("content"), str) or results[-1]["content"].startswith("Error:"):
        problems.append("tool result is not a successful text: " + str(results[-1].get("content"))[:200])
    return problems


def reply(message, finish):
    return {"id": "fake", "object": "chat.completion", "model": "fake",
            "choices": [{"index": 0, "message": message, "finish_reason": finish}]}


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if self.path != "/v1/chat/completions":
            body = {"error": {"message": "wrong path " + self.path}}
        elif any(m.get("role") == "tool" for m in req.get("messages", [])):
            problems = check(req)
            text = "PASS: tool round trip works." if not problems else "FAIL: " + "; ".join(problems)
            print(text)
            body = reply({"role": "assistant", "content": text}, "stop")
        else:
            body = reply({"role": "assistant", "content": None, "tool_calls": [
                {"id": CALL_ID, "type": "function", "function": {"name": "get_scene", "arguments": "{}"}}]}, "tool_calls")
        data = json.dumps(body).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


if __name__ == "__main__":
    print(f"Fake LLM on http://localhost:{PORT}/v1")
    HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()

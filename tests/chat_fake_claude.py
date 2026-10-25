#!/usr/bin/env python3
"""Fake Claude Code CLI to check the chat pane's Claude Code provider without using your plan.

Usage: in BambuStudio's chat pane: Settings... > Provider "Claude (subscription, via Claude Code)",
Claude Code path <repo>/tests/chat_fake_claude.py. Send a message, then a second one.
The first reply asks for the second message; the second reply says PASS or FAIL
(the flags the app passes, and that --resume carries the session id of the first message).
"""
import json
import sys

SESSION = "fake-session-1"
REQUIRED = [["-p"], ["--output-format", "stream-json"], ["--tools", ""], ["--strict-mcp-config"],
            ["--allowedTools", "mcp__bambu"], ["--setting-sources", ""]]


def has(args, flag):
    """True when the args contain flag (a list: the option and its values) in that order."""
    return any(args[i:i + len(flag)] == flag for i in range(len(args)))


def problems(args, prompt):
    found = [" ".join(x or '""' for x in f) for f in REQUIRED if not has(args, f)]
    out = ["missing " + ", ".join(found)] if found else []
    if not has(args, ["--resume", SESSION]):
        out.append("--resume " + SESSION + " missing on the second message")
    if not prompt:
        out.append("no prompt on stdin")
    return out


def emit(event):
    print(json.dumps(event), flush=True)


def main():
    args = sys.argv[1:]
    prompt = sys.stdin.read().strip()
    emit({"type": "system", "subtype": "init", "session_id": SESSION})
    if "--resume" not in args:
        emit({"type": "assistant", "session_id": SESSION, "message": {"content": [
            {"type": "tool_use", "id": "toolu_fake", "name": "mcp__bambu__get_scene", "input": {}}]}})
        text = "First message received. Send another to check --resume."
    else:
        found = problems(args, prompt)
        text = "PASS: Claude Code provider works." if not found else "FAIL: " + "; ".join(found)
    emit({"type": "assistant", "session_id": SESSION, "message": {"content": [{"type": "text", "text": text}]}})
    emit({"type": "result", "subtype": "success", "is_error": False, "result": text, "session_id": SESSION})


if __name__ == "__main__":
    main()

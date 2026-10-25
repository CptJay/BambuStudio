#!/usr/bin/env python3
"""Smoke test for the MCP server in a running BambuStudio: calls every tool once.

Usage: start BambuStudio with an empty plate, then run  python3 tests/mcp_smoke.py
Exits non-zero on the first tool that crashes the app, errors unexpectedly or returns something wrong.
"""
import json
import shutil
import sys
import tempfile
import urllib.request
from pathlib import Path

URL = "http://127.0.0.1:13619/mcp"
TOOLS = {"get_scene", "add_shape", "render_openscad", "get_view", "import_model", "delete_objects", "add_text",
         "export_object", "transform_object", "duplicate_object", "arrange", "slice", "export_gcode"}


def fail(message):
    print(f"FAIL {message}", file=sys.stderr)
    sys.exit(1)


def rpc(method, params=None):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}}).encode()
    try:
        with urllib.request.urlopen(urllib.request.Request(URL, body), timeout=700) as res:
            return json.load(res)
    except OSError as e:
        fail(f"{method}: no response from BambuStudio ({e}). Did it crash?")


def call(tool, args=None, expect_error=False):
    result = rpc("tools/call", {"name": tool, "arguments": args or {}}).get("result")
    if result is None:
        fail(f"{tool}: no result")
    content = result["content"][0]
    if result.get("isError") != expect_error:
        fail(f"{tool} {args}: {'expected an error' if expect_error else content.get('text')}")
    print(f"ok   {tool}")
    return json.loads(content["text"]) if content["type"] == "text" and not expect_error else content


def check(condition, message):
    if not condition:
        fail(f"{message}")


def size(obj):
    return obj["instances"][0]["size"]


def main():
    listed = {t["name"] for t in rpc("tools/list")["result"]["tools"]}
    check(TOOLS <= listed, f"tools/list is missing {TOOLS - listed}")
    check(not call("get_scene")["objects"], "the plate is not empty; this test clears it, so start with an empty plate")

    tmp = Path(tempfile.mkdtemp(prefix="mcp-smoke-"))
    try:
        box = call("add_shape", {"shape": "box", "size": [60, 20, 5], "name": "smoke box"})
        check(size(box) == [60, 20, 5], f"box size {size(box)}")
        moved = call("transform_object", {"name": "smoke box", "position": [100, 100], "rotate": [0, 0, 90]})
        check(round(size(moved)[0]) == 20 and moved["instances"][0]["min"][2] == 0, "transform_object result")
        call("add_text", {"name": "smoke box", "text": "OK", "depth": 0.6})
        call("add_text", {"name": "smoke box", "text": "UP", "mode": "emboss", "position": [100, 85]})
        call("add_text", {"name": "smoke box", "text": "X", "font": "No Such Font 123"}, expect_error=True)
        call("duplicate_object", {"name": "smoke box", "copies": 1})

        stl = tmp / "box.stl"
        call("export_object", {"name": "smoke box", "path": str(stl)})
        check(stl.stat().st_size > 0, "export_object wrote an empty file")
        call("import_model", {"path": str(stl)})
        if shutil.which("openscad"):
            call("render_openscad", {"code": "cylinder(h = 10, d = 15);", "name": "smoke cylinder"})
        else:
            print("skip render_openscad (openscad not installed)")

        image = call("get_view", {"view": "iso", "size": 128})
        check(image["type"] == "image" and image["mimeType"] == "image/png", "get_view did not return a PNG")

        call("arrange")
        stats = call("slice")
        check(stats["filament_g"] > 0, "slice used no filament")
        for name in ("plate.gcode", "plate.gcode.3mf"):
            call("export_gcode", {"path": str(tmp / name)})
            check((tmp / name).stat().st_size > 0, f"export_gcode wrote an empty {name}")

        call("delete_objects", {"names": ["nothing by this name"]}, expect_error=True)
        call("delete_objects", {"names": ["smoke box 2"]})
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        call("delete_objects")
    print("All MCP tools work.")


if __name__ == "__main__":
    main()

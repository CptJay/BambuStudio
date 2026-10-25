#!/usr/bin/env python3
"""Smoke test for the MCP server in a running BambuStudio: calls every tool once.

Usage: start BambuStudio with an empty plate, then run  python3 tests/mcp_smoke.py
Exits non-zero on the first tool that crashes the app, errors unexpectedly or returns something wrong.
Afterwards BambuStudio's current project is a deleted temp file: start a new project (File > New) before saving your work.
"""
import json
import shutil
import sys
import tempfile
import urllib.request
from pathlib import Path

URL = "http://127.0.0.1:13619/mcp"
TOOLS = {"get_scene", "add_shape", "render_openscad", "get_view", "import_model", "delete_objects", "add_text",
         "export_object", "transform_object", "duplicate_object", "arrange", "slice", "export_gcode",
         "get_settings", "set_settings", "select_process_preset", "undo", "save_project", "open_project"}


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
        # Names stay unique, since every tool finds objects by name.
        call("add_shape", {"shape": "box", "size": [5, 5, 5], "name": "twin"})
        call("add_shape", {"shape": "box", "size": [5, 5, 5], "name": "twin"})
        call("duplicate_object", {"name": "twin"})
        names = [o["name"] for o in call("get_scene")["objects"]]
        check(len(names) == len(set(names)) and {"twin", "twin 2", "twin 3"} <= set(names), f"duplicate names: {names}")
        call("delete_objects", {"names": ["twin", "twin 2", "twin 3"]})

        # Settings: only per-object changes and a re-select of the current preset, so the user's presets stay untouched.
        process = call("get_settings")["presets"]["process"]
        if not process.endswith("(modified)"):
            call("select_process_preset", {"preset": process})
        call("select_process_preset", {"preset": "No Such Preset"}, expect_error=True)
        changed = call("set_settings", {"name": "smoke box", "values": {"wall_loops": 3, "sparse_infill_density": "30%"}})
        check(changed["settings"]["wall_loops"] == {"label": "Wall loops", "value": "3", "source": "object"}, f"set_settings {changed}")
        call("set_settings", {"name": "smoke box", "values": {"wall_loops": 4, "sparse_infill_pattern": "spaghetti"}}, expect_error=True)
        check(call("get_settings", {"name": "smoke box", "keys": ["wall_loops"]})["settings"]["wall_loops"]["value"] == "3",
              "a rejected set_settings call still changed wall_loops")
        call("set_settings", {"values": {"nozzle_diameter": "0.6"}}, expect_error=True)
        call("set_settings", {"values": {"post_process": "echo hacked"}}, expect_error=True)

        stl = tmp / "box.stl"
        call("export_object", {"name": "smoke box", "path": str(stl)})
        check(stl.stat().st_size > 0, "export_object wrote an empty file")
        call("import_model", {"path": str(stl)})
        if shutil.which("openscad"):
            call("render_openscad", {"code": "cylinder(h = 10, d = 15);", "name": "smoke cylinder"})
            # A cube floating above another one: slicing must report it, so a broken warnings list can't pass as [].
            call("render_openscad", {"code": "cube(10); translate([0, 0, 25]) cube(10);", "name": "smoke floating"})
            warnings = call("slice")["warnings"]
            check(any(w.get("object") == "smoke floating" or "smoke floating" in w["message"] for w in warnings),
                  f"no warning for the floating cube: {warnings}")
            call("delete_objects", {"names": ["smoke floating"]})
        else:
            print("skip render_openscad (openscad not installed)")

        image = call("get_view", {"view": "iso", "size": 128})
        check(image["type"] == "image" and image["mimeType"] == "image/png", "get_view did not return a PNG")

        call("arrange")
        stats = call("slice")
        check(stats["filament_g"] > 0 and isinstance(stats["warnings"], list), f"slice result {stats}")
        # Undo right after slicing: the app is on the Preview tab then.
        call("add_shape", {"shape": "box", "size": [5, 5, 5], "name": "undo me"})
        call("slice")
        undone = call("undo")
        check(undone["undone"] == "Add Primitive" and all(o["name"] != "undo me" for o in undone["objects"]), f"undo {undone}")
        for name in ("plate.gcode", "plate.gcode.3mf"):
            call("export_gcode", {"path": str(tmp / name)})
            check((tmp / name).stat().st_size > 0, f"export_gcode wrote an empty {name}")

        project = str(tmp / "smoke.3mf")
        call("save_project", {"path": project})
        call("save_project", {"path": project}, expect_error=True)
        reopened = call("open_project", {"path": project})
        check(any(o["name"] == "smoke box" for o in reopened["objects"]), f"open_project lost objects: {reopened}")
        call("add_shape", {"shape": "box", "size": [5, 5, 5], "name": "unsaved"})
        call("open_project", {"path": project}, expect_error=True)
        call("delete_objects", {"names": ["unsaved"]})
        # Not tested here: slice refusing a project with post_process scripts. Opening such a project leaves its
        # script in the presets, and tools cannot discard that, so the test would leave the app unsafe.

        call("delete_objects", {"names": ["nothing by this name"]}, expect_error=True)
        call("delete_objects", {"names": ["smoke box 2"]})
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        call("delete_objects")
    print("All MCP tools work.")
    print("Start a new project in BambuStudio (File > New) before saving your own work: the test left a deleted temp file as the current project.")


if __name__ == "__main__":
    main()

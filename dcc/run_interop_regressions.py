"""Unified UE 5.8+ / Blender 5.2+ LightUSD regression orchestrator.

The JSON configuration uses argument arrays, never shell strings. Placeholders
available to UE/background commands are: ``{bridge_url}``, ``{bridge_token}``,
``{asset_id}``, ``{repo}``, and ``{output}``.
"""

from __future__ import annotations

import argparse
import json
import os
import secrets
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from dcc.bridge.asset_bridge import BridgeServer, upload_asset_bundle_http


DEFAULT_BLENDER_TESTS = (
    "dcc/blender/tests/usdskel_roundtrip.py",
    "dcc/blender/tests/rigify_metarig.py",
    "dcc/blender/tests/rigify_retarget.py",
    "dcc/blender/tests/roundtrip.py",
    "dcc/blender/tests/materialx_channels.py",
)


def _expand(command, values):
    return [str(item).format_map(values) for item in command]


def _run(name, command, environment, timeout, report, redact=()):
    started = time.monotonic()
    process = subprocess.run(command, cwd=ROOT, env=environment, text=True,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             timeout=timeout, check=False)
    safe_command = ["<redacted>" if item in redact else item for item in command]
    result = {"name": name, "command": safe_command, "exit_code": process.returncode,
              "seconds": round(time.monotonic() - started, 3),
              "output": process.stdout[-16000:]}
    report["steps"].append(result)
    if process.returncode:
        raise RuntimeError(f"{name} failed with exit code {process.returncode}")
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", help="JSON configuration; defaults to Blender-only coverage")
    parser.add_argument("--report", default="/tmp/lightusd-interop-report.json")
    parser.add_argument("--timeout", type=int, default=900, help="per-step timeout in seconds")
    args = parser.parse_args(argv)
    config = json.loads(Path(args.config).read_text()) if args.config else {}
    blender = config.get("blender", os.environ.get("BLENDER", "blender"))
    output = Path(config.get("output", "/tmp/lightusd-interop"))
    output.mkdir(parents=True, exist_ok=True)
    token = config.get("bridge_token") or secrets.token_urlsafe(24)
    bridge_root = tempfile.mkdtemp(prefix="lightusd-bridge-")
    server = BridgeServer(bridge_root, config.get("bridge_host", "127.0.0.1"),
                          int(config.get("bridge_port", 0)), token=token)
    report = {"format": "lightusd-interop-report-v1", "succeeded": False,
              "ue_minimum": "5.8", "blender_minimum": "5.2", "steps": []}
    background = []
    try:
        server.start()
        local_url = f"http://127.0.0.1:{server.address[1]}"
        advertised_url = config.get("bridge_url", local_url)
        environment = os.environ.copy()
        environment["LIGHTUSD_REPO_ROOT"] = str(ROOT)
        environment["LIGHTUSD_BLENDER_USDSKEL_OUT"] = str(output / "usdskel")
        for relative in config.get("blender_tests", DEFAULT_BLENDER_TESTS):
            _run("blender:" + Path(relative).stem,
                 [blender, "--background", "--python", str(ROOT / relative)],
                 environment, args.timeout, report)

        root_layer = output / "usdskel" / "usdskel_roundtrip.usda"
        bundle_root = Path(config.get("bundle_root", root_layer.parent))
        uploaded = upload_asset_bundle_http(
            local_url, root_layer, bundle_root, token,
            "blender-to-ue.lusdbundle",
            strict=bool(config.get("strict_dependencies", True)),
            additional_paths=config.get("bundle_dependencies", []))
        report["input_bundle"] = uploaded
        values = {"bridge_url": advertised_url, "bridge_token": token,
                  "asset_id": uploaded["id"], "repo": str(ROOT),
                  "output": str(output), "bridge_port": str(server.address[1])}
        for index, command in enumerate(config.get("background_commands", [])):
            background.append(subprocess.Popen(_expand(command, values), cwd=ROOT,
                                               env=environment, stdout=subprocess.PIPE,
                                               stderr=subprocess.STDOUT, text=True))
        if background:
            time.sleep(1.0)
            failed = [process for process in background if process.poll() is not None]
            if failed:
                output_text = failed[0].stdout.read() if failed[0].stdout else ""
                raise RuntimeError("background command failed: " + output_text[-4000:])
        for index, command in enumerate(config.get("ue_commands", [])):
            result = _run(f"ue:{index + 1}", _expand(command, values), environment,
                          args.timeout, report, redact=(token,))
            marker = "LIGHTUSD_UE_REPORT="
            for line in result["output"].splitlines():
                if line.startswith(marker):
                    result["ue_report"] = json.loads(line[len(marker):])
            if config.get("ue_commands") and "ue_report" not in result:
                raise RuntimeError(f"ue:{index + 1} produced no verified UE report")
        report["succeeded"] = True
    except Exception as exc:
        report["error"] = str(exc)
    finally:
        for process in reversed(background):
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
        server.stop()
        Path(args.report).parent.mkdir(parents=True, exist_ok=True)
        Path(args.report).write_text(json.dumps(report, indent=2), encoding="utf-8")
    return 0 if report["succeeded"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

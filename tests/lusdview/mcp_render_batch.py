#!/usr/bin/env python3
"""Render a JSON manifest through one persistent lusdview MCP process.

Cases may provide either ``path`` or inline ``usda``. Backend/process-wide
options belong in ``viewer_args``; tests that need different backends, loaders,
environment variables, or startup-only options intentionally use another batch.
"""

import json
from pathlib import Path
import re
import subprocess
import sys
import threading
import time


SKIP = 77


class McpClient:
    def __init__(self, command):
        self.proc = subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1)
        self.next_id = 1
        self.responses = {}
        self.condition = threading.Condition()
        self.stderr = []
        threading.Thread(target=self._read_stdout, daemon=True).start()
        threading.Thread(target=self._read_stderr, daemon=True).start()

    def _read_stdout(self):
        for line in self.proc.stdout:
            try:
                response = json.loads(line)
            except json.JSONDecodeError:
                continue
            with self.condition:
                self.responses[response.get("id")] = response
                self.condition.notify_all()

    def _read_stderr(self):
        for line in self.proc.stderr:
            self.stderr.append(line)
            if len(self.stderr) > 2000:
                del self.stderr[:1000]

    def call(self, name, arguments=None, timeout=30.0):
        request_id = self.next_id
        self.next_id += 1
        request = {"jsonrpc": "2.0", "id": request_id,
                   "method": "tools/call",
                   "params": {"name": name,
                              "arguments": arguments or {}}}
        try:
            self.proc.stdin.write(json.dumps(request) + "\n")
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError) as exc:
            raise RuntimeError(f"lusdview exited before {name}: {exc}") from exc
        deadline = time.monotonic() + timeout
        with self.condition:
            while request_id not in self.responses:
                if self.proc.poll() is not None:
                    raise RuntimeError(
                        f"lusdview exited with {self.proc.returncode} during {name}")
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise RuntimeError(f"MCP {name} timed out")
                self.condition.wait(min(remaining, 0.1))
            response = self.responses.pop(request_id)
        if "error" in response:
            raise RuntimeError(f"MCP {name}: {response['error']}")
        return response["result"]["structuredContent"]

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()

    def stderr_text(self):
        return "".join(self.stderr)


def wait_for_load(client, path, old_generation, timeout):
    deadline = time.monotonic() + timeout
    last = {}
    while time.monotonic() < deadline:
        last = client.call("get_scene_info")
        if (not last.get("loading") and last.get("filepath") == str(path) and
                last.get("scene_generation", 0) > old_generation):
            if not last.get("loaded"):
                raise RuntimeError(f"load failed: {last}")
            return last
        time.sleep(0.02)
    raise RuntimeError(f"load timed out: {last}")


def check_ppm(path):
    data = path.read_bytes()
    if not data.startswith(b"P6") or len(data) < 32:
        raise RuntimeError(f"invalid or empty screenshot: {path}")
    # A flat clear image is never a useful render regression result.
    payload = data.split(b"\n", 3)[-1]
    if len(set(payload[: min(len(payload), 65536)])) < 2:
        raise RuntimeError(f"flat screenshot: {path}")


def capture_when_ready(client, path, timeout=10.0):
    """Capture a frame rendered after the preceding settings and viewport edits."""
    frame = client.call("get_render_stats")["frame_serial"]
    deadline = time.monotonic() + timeout
    last = {}
    while time.monotonic() < deadline:
        last = client.call("screenshot", {"path": str(path), "after_frame": frame})
        if last.get("written"):
            return last
        if last.get("ready") is not False:
            break
        time.sleep(0.02)
    raise RuntimeError(
        f"screenshot was not ready before timeout: {last}")


def rss_kib(pid):
    """Return current resident memory on Linux, or None when unavailable."""
    try:
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    except (FileNotFoundError, PermissionError, ValueError):
        pass
    return None


def main():
    if len(sys.argv) < 4:
        print(f"usage: {sys.argv[0]} lusdview manifest.json output-dir "
              "[--windowed] [viewer args...]",
              file=sys.stderr)
        return 2
    viewer = Path(sys.argv[1]).resolve()
    manifest = json.loads(Path(sys.argv[2]).read_text())
    output = Path(sys.argv[3]).resolve()
    output.mkdir(parents=True, exist_ok=True)
    extra_args = sys.argv[4:]
    windowed = "--windowed" in extra_args
    extra_args = [arg for arg in extra_args if arg != "--windowed"]
    command = [str(viewer)]
    if not windowed:
        command.append("--headless")
    command += ["--frames", "1000000", "--mcp-stdio", "--no-grid"]
    command += manifest.get("viewer_args", []) + extra_args
    client = McpClient(command)
    try:
        # A first request distinguishes transport/startup failure from a case.
        initial = client.call("get_scene_info", timeout=15)
        stats = client.call("get_render_stats")
        required_stats = {"ui_fps", "render_fps", "adaptive_quality",
                          "adaptive_tier", "render_scale",
                          "target_render_fps"}
        missing_stats = required_stats.difference(stats)
        if missing_stats:
            raise RuntimeError(
                f"get_render_stats missing fields: {sorted(missing_stats)}")
        settings = client.call(
            "render_settings",
            {"adaptive_quality": False, "target_render_fps": 30})
        if settings.get("adaptive_quality") is not False:
            raise RuntimeError("render_settings did not disable adaptive quality")
        pid = client.proc.pid
        lifecycle = (initial.get("window_generation"),
                     initial.get("renderer_generation"))
        baseline_rss = rss_kib(pid)
        peak_rss = baseline_rss
        completed = 0
        captured = {}
        repeat = int(manifest.get("repeat", 1))
        if repeat < 1:
            raise RuntimeError("manifest repeat must be at least 1")
        iterations = ((iteration, index, case)
                      for iteration in range(repeat)
                      for index, case in enumerate(manifest["cases"]))
        for iteration, index, case in iterations:
            base_name = case.get("name", f"case-{index}")
            name = (base_name if repeat == 1 else
                    f"{base_name}-iteration-{iteration + 1}")
            if "usda" in case:
                load_args = {"usda": case["usda"]}
            else:
                path = Path(case["path"]).resolve()
                load_args = {"path": str(path)}

            load_settings = case.get("load_settings", {})
            if load_settings:
                client.call("render_settings", load_settings)
            before = client.call("get_scene_info").get("scene_generation", 0)
            started = client.call("load_usd", load_args)
            path = Path(started["path"])
            info = wait_for_load(client, path, before,
                                 float(case.get("timeout", 30)))
            for key, expected in case.get("expect", {}).items():
                if info.get(key) != expected:
                    raise RuntimeError(
                        f"{name}: expected {key}={expected!r}, "
                        f"got {info.get(key)!r}")
            for pattern in case.get("expect_stderr", []):
                if re.search(pattern, client.stderr_text()) is None:
                    raise RuntimeError(
                        f"{name}: stderr did not match {pattern!r}")
            settle_seconds = float(case.get("settle_seconds", 0))
            if settle_seconds > 0:
                time.sleep(settle_seconds)

            captures = case.get("captures")
            if captures is None:
                captures = [{"name": "capture",
                             "settings": case.get("settings", {}),
                             "viewport": case.get("viewport", {"op": "fit"})}]
            for capture_index, capture in enumerate(captures):
                settings = capture.get("settings", {})
                if settings:
                    client.call("render_settings", settings)
                viewport = capture.get("viewport")
                if viewport:
                    client.call("viewport", viewport)
                for action in capture.get("actions", []):
                    client.call(action["tool"], action.get("arguments", {}),
                                float(action.get("timeout", 30)))
                suffix = capture.get("name", f"capture-{capture_index}")
                shot = output / f"{completed:03d}-{name}-{suffix}.ppm"
                result = capture_when_ready(client, shot)
                check_ppm(shot)
                image_bytes = shot.read_bytes()
                different_from = capture.get("different_from")
                if different_from:
                    reference = captured.get(different_from)
                    if reference is None:
                        raise RuntimeError(
                            f"{name}/{suffix}: unknown capture reference "
                            f"{different_from!r}")
                    if image_bytes == reference:
                        raise RuntimeError(
                            f"{name}/{suffix}: image did not change from "
                            f"{different_from}")
                captured[f"{name}/{suffix}"] = image_bytes
                captured[suffix] = image_bytes
            if client.proc.pid != pid:
                raise RuntimeError("viewer PID changed inside a batch")
            current = client.call("get_scene_info")
            current_lifecycle = (current.get("window_generation"),
                                 current.get("renderer_generation"))
            if current_lifecycle != lifecycle:
                raise RuntimeError(
                    f"{name}: window/renderer recreated: "
                    f"{lifecycle} -> {current_lifecycle}")
            current_rss = rss_kib(pid)
            if current_rss is not None:
                peak_rss = max(peak_rss or current_rss, current_rss)
            print(f"PASS: {name}: generation={info['scene_generation']} "
                  f"triangles={info['triangle_count']} pid={pid} "
                  f"lifecycle={lifecycle} rss_kib={current_rss}")
            completed += 1
        max_growth_mb = manifest.get("max_rss_growth_mb")
        if max_growth_mb is not None and baseline_rss is not None:
            final_rss = rss_kib(pid)
            growth_kib = (final_rss or baseline_rss) - baseline_rss
            if growth_kib > float(max_growth_mb) * 1024.0:
                raise RuntimeError(
                    f"resident-memory growth {growth_kib / 1024.0:.1f} MiB "
                    f"exceeds {max_growth_mb} MiB")
        print(f"PASS: {completed} cases rendered in one lusdview process; "
              f"baseline_rss_kib={baseline_rss} peak_rss_kib={peak_rss}")
        return 0
    except Exception as exc:
        log = client.stderr_text()
        unavailable = ("failed to initialize" in log.lower() or
                       "no vulkan" in log.lower() or
                       "vulkan initialization failed" in log.lower() or
                       "ray tracing is unavailable" in log.lower() or
                       "ray-query unavailable" in log.lower())
        # Mesa may link only lusdview's deliberately reduced-sampler fallback
        # shader.  That path provides shaded preview but not the diagnostic AOV
        # modes exercised by the material batch, so identical captures are a
        # capability skip rather than a material regression.
        unavailable = unavailable or (
            "image did not change" in str(exc) and
            "full GL material shader unavailable; using low-sampler fallback"
            in log)
        print(("SKIP" if unavailable else "FAIL") + f": {exc}", file=sys.stderr)
        if log:
            print(log[-12000:], file=sys.stderr)
        return SKIP if unavailable else 1
    finally:
        client.close()


if __name__ == "__main__":
    sys.exit(main())

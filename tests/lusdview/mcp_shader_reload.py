#!/usr/bin/env python3
"""Transactional shader/kernel reload regression in one persistent viewer."""

import argparse
import shutil
import sys
import tempfile
import time
from pathlib import Path

from mcp_render_batch import McpClient, SKIP


def backend_state(status, backend):
    for state in status.get("backends", []):
        if state.get("backend") == backend:
            return state
    raise RuntimeError(f"shader_reload status omitted {backend}: {status}")


def wait_reload(client, backend, predicate, timeout=180.0,
                ignore_error_through_attempt=None):
    deadline = time.monotonic() + timeout
    last = {}
    while time.monotonic() < deadline:
        last = backend_state(client.call("shader_reload", {"action": "status"}),
                             backend)
        if not last.get("pending"):
            if predicate(last):
                return last
            # A failed first compile can never satisfy a success predicate.
            # Return the compiler error immediately instead of waiting for the
            # full polling timeout and making an ordinary compile failure look
            # like an MCP/Vulkan deadlock.
            error_is_current = (
                ignore_error_through_attempt is None or
                int(last.get("attempts", 0)) > ignore_error_through_attempt)
            if last.get("last_error") and error_is_current:
                raise RuntimeError(f"shader reload failed: {last}")
        time.sleep(0.05)
    raise RuntimeError(f"shader reload timed out: {last}")


def wait_loaded(client, timeout=180.0):
    deadline = time.monotonic() + timeout
    last = {}
    while time.monotonic() < deadline:
        last = client.call("get_scene_info", timeout=timeout)
        if not last.get("loading"):
            if last.get("loaded"):
                return last
            if last.get("filepath"):
                raise RuntimeError(f"scene load failed: {last}")
        time.sleep(0.05)
    raise RuntimeError(f"scene load timed out: {last}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("viewer", type=Path)
    parser.add_argument("backend", choices=("vulkan", "cuda", "hip"))
    parser.add_argument("source", type=Path)
    parser.add_argument("scene", type=Path)
    parser.add_argument("--vk-device")
    parser.add_argument("--materialx-vk-shader-max-kib", type=int)
    parser.add_argument("--async-smoke", action="store_true",
                        help="assert non-blocking Vulkan launch only")
    args = parser.parse_args()

    viewer = args.viewer.resolve()
    source = args.source.resolve()
    scene = args.scene.resolve()
    # Idle headless frames can spin very quickly while the driver compiles.
    # Let the RPC/CTest deadlines, not the frame budget, bound this process.
    command = [str(viewer), "--headless", "--frames", "1000000000",
               "--mcp-stdio", "--no-grid", "--size", "64x64"]
    if args.backend == "vulkan":
        command += ["--backend", "vk", "--rt"]
        if args.vk_device:
            command += ["--vk-device", args.vk_device]
        if args.materialx_vk_shader_max_kib:
            command += ["--materialx-vk-shader-max-kib",
                        str(args.materialx_vk_shader_max_kib)]
    else:
        command += [f"--{args.backend}", "--path-trace", "--pt-samples", "1"]
    command.append(str(scene))

    client = McpClient(command)
    try:
        wait_loaded(client)
        if args.backend == "vulkan" and source.name == "raytrace.comp":
            # This case exercises the hardware ray-query shader.  A cold RT
            # pipeline may deliberately fall back to compute-BVH, and CPU
            # Vulkan implementations can advertise ray-query extensions while
            # still being unsuitable for this long-running regression.  The
            # SWBVH test covers the fallback shader separately.
            startup = client.stderr_text().lower()
            if ("rt=software" in startup or
                    "compute-bvh fallback" in startup or
                    "device=cpu" in startup):
                print("SKIP: Vulkan hardware ray-query path unavailable; "
                      "use the SWBVH live-reload test", file=sys.stderr)
                return SKIP
        initial_info = client.call("get_scene_info")
        lifecycle = (initial_info.get("window_generation"),
                     initial_info.get("renderer_generation"))
        initial = backend_state(
            client.call("shader_reload", {"action": "status"}), args.backend)
        initial_generation = int(initial.get("generation", 0))
        initial_successes = int(initial.get("successes", 0))

        with tempfile.TemporaryDirectory(prefix="lusdview-live-reload-") as td:
            td = Path(td)
            good = td / source.name
            bad = td / (source.stem + "-bad" + source.suffix)
            shutil.copyfile(source, good)
            if args.backend == "vulkan" and source.name == "raytrace.comp":
                # This test launches interactive RT, not the production path
                # tracer. Fail even on a warm cache if reload loses that mode.
                with good.open("a", encoding="utf-8") as stream:
                    stream.write(
                        "\n#if !defined(LUSDVIEW_RT_INTERACTIVE_ONLY) || "
                        "!LUSDVIEW_RT_INTERACTIVE_ONLY\n"
                        '#error "interactive reload included production integrator"\n'
                        "#endif\n")
            if args.backend == "vulkan":
                bad.write_text("#version 460\nthis_is_not_valid_glsl\n")
            else:
                bad.write_text('extern "C" __global__ void not_trace() { broken }\n')

            launch = client.call("shader_reload",
                                 {"action": "reload", "backend": args.backend,
                                  "source": str(good)},
                                 timeout=10.0 if args.backend == "vulkan" else 180.0)
            launch_state = backend_state(launch, args.backend)
            if args.backend == "vulkan":
                if not launch_state.get("pending"):
                    raise RuntimeError(
                        f"Vulkan reload did not return pending: {launch_state}")
                if launch_state.get("phase") not in ("compiling", "warming"):
                    raise RuntimeError(
                        f"Vulkan reload omitted async phase: {launch_state}")
                # The old pipeline must keep the persistent process responsive
                # while GLSL/driver work happens in the background.
                responsive = client.call("get_scene_info", timeout=10.0)
                if not responsive.get("loaded"):
                    raise RuntimeError(
                        f"viewer became unavailable during reload: {responsive}")
                if args.async_smoke:
                    print("PASS: Vulkan reload returned pending and viewer remained responsive")
                    return 0
            good_state = wait_reload(
                client, args.backend,
                lambda s: int(s.get("successes", 0)) > initial_successes)
            if int(good_state.get("generation", 0)) <= initial_generation:
                raise RuntimeError(f"successful reload did not advance generation: {good_state}")
            committed_generation = int(good_state["generation"])
            committed_successes = int(good_state["successes"])

            try:
                client.call("shader_reload",
                            {"action": "reload", "backend": args.backend,
                             "source": str(bad)}, timeout=180.0)
            except RuntimeError:
                pass
            failed = wait_reload(
                client, args.backend,
                lambda s: bool(s.get("last_error")), timeout=30.0)
            if int(failed.get("generation", 0)) != committed_generation:
                raise RuntimeError(f"failed reload replaced the working module: {failed}")
            if int(failed.get("successes", 0)) != committed_successes:
                raise RuntimeError(f"failed reload advanced success count: {failed}")

            watched = td / source.name
            shutil.copyfile(source, watched)
            client.call("shader_reload",
                        {"action": "watch", "backend": args.backend,
                         "source": str(watched), "watch": True})
            before_watch = backend_state(
                client.call("shader_reload", {"action": "status"}), args.backend)
            watch_attempts = int(before_watch.get("attempts", 0))
            watch_successes = int(before_watch.get("successes", 0))
            time.sleep(0.35)
            with watched.open("a", encoding="utf-8") as stream:
                stream.write("\n// mcp live-watch regression edit\n")
            watch_state = wait_reload(
                client, args.backend,
                lambda s: (int(s.get("attempts", 0)) > watch_attempts and
                           int(s.get("successes", 0)) > watch_successes),
                timeout=180.0,
                ignore_error_through_attempt=watch_attempts)
            client.call("shader_reload",
                        {"action": "watch", "backend": args.backend,
                         "watch": False})

        final_info = client.call("get_scene_info")
        final_lifecycle = (final_info.get("window_generation"),
                           final_info.get("renderer_generation"))
        if final_lifecycle != lifecycle:
            raise RuntimeError(f"reload recreated viewer/renderer: {lifecycle} -> {final_lifecycle}")
        if client.proc.pid <= 0:
            raise RuntimeError("persistent viewer process disappeared")
        print(f"PASS: {args.backend} transactional reload, rollback, and watch "
              f"in pid={client.proc.pid}; generation={watch_state['generation']}")
        return 0
    except Exception as exc:
        log = client.stderr_text()
        unavailable_tokens = ("unavailable", "no cuda", "no hip", "no vulkan",
                              "no rocm-capable device",
                              "ray query unsupported", "failed to initialize",
                              "pipeline cache is cold",
                              "rt=software")
        unavailable = any(token in log.lower() for token in unavailable_tokens)
        print(("SKIP" if unavailable else "FAIL") + f": {exc}", file=sys.stderr)
        if log:
            print(log[-16000:], file=sys.stderr)
        return SKIP if unavailable else 1
    finally:
        client.close()


if __name__ == "__main__":
    sys.exit(main())

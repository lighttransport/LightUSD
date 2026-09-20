#!/usr/bin/env python3
"""Render an authored USD stereo camera pair to a side-by-side PPM."""

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile


def parse_size(value):
    try:
        width, height = (int(part) for part in value.lower().split("x", 1))
    except (TypeError, ValueError):
        raise argparse.ArgumentTypeError("size must be WIDTHxHEIGHT")
    if width < 1 or height < 1:
        raise argparse.ArgumentTypeError("size dimensions must be positive")
    return width, height


def run(command):
    completed = subprocess.run(command, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True)
    if completed.returncode:
        sys.stderr.write(completed.stdout)
        raise SystemExit(completed.returncode)
    return completed.stdout


def read_ppm(path):
    data = pathlib.Path(path).read_bytes()
    index = 0

    def token():
        nonlocal index
        while index < len(data):
            if data[index:index + 1] == b"#":
                index = data.find(b"\n", index)
                if index < 0:
                    raise ValueError("unterminated PPM comment")
            if not chr(data[index]).isspace():
                break
            index += 1
        start = index
        while index < len(data) and not chr(data[index]).isspace():
            index += 1
        return data[start:index]

    if token() != b"P6":
        raise ValueError("expected binary P6 PPM")
    width, height, maximum = int(token()), int(token()), int(token())
    if maximum != 255:
        raise ValueError("only 8-bit PPM input is supported")
    if data[index:index + 2] == b"\r\n":
        index += 2
    elif index < len(data) and chr(data[index]).isspace():
        index += 1
    else:
        raise ValueError("PPM header is missing its pixel delimiter")
    pixels = data[index:]
    if len(pixels) != width * height * 3:
        raise ValueError("PPM payload size does not match its header")
    return width, height, pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scene")
    parser.add_argument("--lusdview", default="build_ninja/lusdview")
    parser.add_argument("--output", required=True)
    parser.add_argument("--size", type=parse_size, default=(640, 480),
                        help="per-eye resolution, default 640x480")
    parser.add_argument("--camera", help="select either eye when pairs are ambiguous")
    parser.add_argument("--backend", choices=("gl", "vk"), default="vk")
    parser.add_argument("--next", action="store_true", dest="next_loader")
    parser.add_argument("--viewer-arg", action="append", default=[],
                        help="additional lusdview argument; repeat as needed")
    args = parser.parse_args()
    binary = pathlib.Path(args.lusdview)
    if not binary.is_file():
        parser.error(f"lusdview executable not found: {binary}")
    scene = pathlib.Path(args.scene)
    if not scene.is_file():
        parser.error(f"scene not found: {scene}")
    eye_width, eye_height = args.size

    with tempfile.TemporaryDirectory(prefix="lusdview-stereo-") as temp:
        temp = pathlib.Path(temp)
        report = temp / "pair.json"
        common = [str(binary), "--headless", "--backend", args.backend,
                  "--frames", "1"]
        if args.next_loader:
            common.append("--next")
        probe = common + ["--size", "32x32", "--stereo",
                          "--render-report", str(report)]
        if args.camera:
            probe += ["--camera", args.camera]
        probe += args.viewer_arg + [str(scene)]
        run(probe)
        stereo = json.loads(report.read_text(encoding="utf-8")).get("stereo", {})
        if not stereo.get("resolved"):
            raise SystemExit("stereo pair resolution failed: " +
                             stereo.get("error", "unknown error"))

        images = []
        for role in ("left", "right"):
            image = temp / f"{role}.ppm"
            command = common + ["--size", f"{eye_width}x{eye_height}",
                                "--camera", stereo[role], "--screenshot",
                                str(image)] + args.viewer_arg + [str(scene)]
            run(command)
            images.append(read_ppm(image))
        if images[0][:2] != images[1][:2]:
            raise SystemExit("left and right renders have different dimensions")
        left, right = images[0][2], images[1][2]
        row_bytes = eye_width * 3
        output = bytearray()
        for y in range(eye_height):
            begin = y * row_bytes
            end = begin + row_bytes
            output += left[begin:end]
            output += right[begin:end]
        target = pathlib.Path(args.output)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(
            f"P6\n{eye_width * 2} {eye_height}\n255\n".encode("ascii") + output)
        print(f"wrote {target} ({eye_width * 2}x{eye_height}, side-by-side)")


if __name__ == "__main__":
    main()

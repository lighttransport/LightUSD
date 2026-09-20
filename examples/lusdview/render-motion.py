#!/usr/bin/env python3
"""Render authored shutter motion through lusdview's raster backends."""

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


def srgb_to_linear(value):
    value /= 255.0
    return value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4


def linear_to_srgb(value):
    value = max(0.0, min(1.0, value))
    value = 12.92 * value if value <= 0.0031308 else 1.055 * value ** (1.0 / 2.4) - 0.055
    return max(0, min(255, round(value * 255.0)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scene")
    parser.add_argument("--lusdview", default="build_ninja/lusdview")
    parser.add_argument("--output", required=True)
    parser.add_argument("--size", type=parse_size, default=(640, 480))
    parser.add_argument("--time", type=float, default=0.0)
    parser.add_argument("--segments", type=int, default=2)
    parser.add_argument("--camera", required=True,
                        help="authored camera providing shutter and per-time pose")
    parser.add_argument("--backend", choices=("gl", "vk"), default="vk")
    parser.add_argument("--next", action="store_true", dest="next_loader")
    parser.add_argument("--viewer-arg", action="append", default=[])
    args = parser.parse_args()
    if args.segments < 1 or args.segments > 64:
        parser.error("--segments must be between 1 and 64")
    binary = pathlib.Path(args.lusdview)
    scene = pathlib.Path(args.scene)
    if not binary.is_file():
        parser.error(f"lusdview executable not found: {binary}")
    if not scene.is_file():
        parser.error(f"scene not found: {scene}")
    width, height = args.size

    with tempfile.TemporaryDirectory(prefix="lusdview-motion-") as temp_name:
        temp = pathlib.Path(temp_name)
        common = [str(binary), "--headless", "--backend", args.backend,
                  "--frames", "1", "--size", f"{width}x{height}",
                  "--camera", args.camera, "--no-raster-motion"]
        if args.next_loader:
            common.append("--next")
        report_path = temp / "report.json"
        run(common + ["--time", repr(args.time), "--render-report",
                      str(report_path)] + args.viewer_arg + [str(scene)])
        shutter = json.loads(report_path.read_text(encoding="utf-8")).get(
            "camera_shutter", {})
        shutter_open = float(shutter.get("open", 0.0))
        shutter_close = float(shutter.get("close", 0.0))
        enabled = bool(shutter.get("enabled")) and shutter_close > shutter_open
        count = args.segments if enabled else 1
        times = [args.time] if count == 1 else [
            args.time + shutter_open + (index + 0.5) *
            (shutter_close - shutter_open) / count
            for index in range(count)]

        accumulation = None
        dimensions = None
        for index, sample_time in enumerate(times):
            image = temp / f"sample-{index}.ppm"
            run(common + ["--time", repr(sample_time), "--screenshot",
                          str(image)] + args.viewer_arg + [str(scene)])
            sample_width, sample_height, pixels = read_ppm(image)
            if dimensions is None:
                dimensions = sample_width, sample_height
                accumulation = [0.0] * len(pixels)
            elif dimensions != (sample_width, sample_height):
                raise SystemExit("motion samples have different dimensions")
            for pixel, value in enumerate(pixels):
                accumulation[pixel] += srgb_to_linear(value)

        output = bytes(linear_to_srgb(value / count) for value in accumulation)
        target = pathlib.Path(args.output)
        target.parent.mkdir(parents=True, exist_ok=True)
        output_width, output_height = dimensions
        target.write_bytes(
            f"P6\n{output_width} {output_height}\n255\n".encode("ascii") + output)
        interval = f"{args.time + shutter_open:g}..{args.time + shutter_close:g}"
        print(f"wrote {target} ({output_width}x{output_height}, "
              f"{count} shutter sample(s), {interval})")


if __name__ == "__main__":
    main()

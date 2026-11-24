#!/usr/bin/env python3
"""Render/pixel regression for color management on the core CPU path.

The fixture authors channel-distinct sRGB (0.25, 0.5, 0.75), selects AP1 as the
rendering working space, and expects both loader paths to reach the same display
pixel. Unlike a neutral gray, this catches transposed matrices and RGB channel
swizzles. The byte reference includes lusdrender's fixed preview exposure/tone
curve; a small tolerance keeps the test portable across scalar/SIMD math.
"""

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"not a PNG: {path}")
    pos, payload = 8, bytearray()
    width = height = depth = color_type = 0
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, color_type = struct.unpack(">IIBB", chunk[:10])
        elif kind == b"IDAT":
            payload.extend(chunk)
        elif kind == b"IEND":
            break
    if depth != 8 or color_type not in (2, 6):
        raise ValueError(f"unsupported PNG layout: depth={depth}, type={color_type}")
    channels = 3 if color_type == 2 else 4
    stride = width * channels
    raw = zlib.decompress(payload)
    rows, previous, offset = [], bytearray(stride), 0
    for _ in range(height):
        filter_type = raw[offset]
        offset += 1
        row = bytearray(raw[offset:offset + stride])
        offset += stride
        for i in range(stride):
            a = row[i - channels] if i >= channels else 0
            b = previous[i]
            c = previous[i - channels] if i >= channels else 0
            if filter_type == 1:
                row[i] = (row[i] + a) & 255
            elif filter_type == 2:
                row[i] = (row[i] + b) & 255
            elif filter_type == 3:
                row[i] = (row[i] + (a + b) // 2) & 255
            elif filter_type == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                predictor = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                row[i] = (row[i] + predictor) & 255
            elif filter_type != 0:
                raise ValueError(f"unsupported PNG filter: {filter_type}")
        rows.append(row)
        previous = row
    return width, height, channels, rows


def center_rgb(path):
    width, height, channels, rows = read_png(path)
    offset = (width // 2) * channels
    row = rows[height // 2]
    return [row[offset], row[offset + 1], row[offset + 2]]


def compare_pixels(path_a, path_b):
    wa, ha, ca, rows_a = read_png(path_a)
    wb, hb, cb, rows_b = read_png(path_b)
    if (wa, ha) != (wb, hb):
        raise ValueError("pixel comparison requires equal image dimensions")
    changed = total = maximum = 0
    max_at = [0, 0]
    max_a = max_b = [0, 0, 0]
    for y in range(ha):
        for x in range(wa):
            oa, ob = x * ca, x * cb
            a = list(rows_a[y][oa:oa + 3])
            b = list(rows_b[y][ob:ob + 3])
            delta = max(abs(av - bv) for av, bv in zip(a, b))
            if delta:
                changed += 1
                total += delta
            if delta > maximum:
                maximum = delta
                max_at = [x, y]
                max_a, max_b = a, b
    return {"changedPixels": changed, "sumMaxChannelDelta": total,
            "maxDelta": maximum, "maxAt": max_at,
            "pixelA": max_a, "pixelB": max_b}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("lusdrender")
    parser.add_argument("repo_root")
    parser.add_argument("--output")
    parser.add_argument("--tolerance", type=int, default=3)
    args = parser.parse_args()
    fixture = os.path.join(args.repo_root, "tests", "usda",
                           "colorspace-render.usda")
    if not os.path.isfile(args.lusdrender) or not os.path.isfile(fixture):
        print("SKIP: lusdrender or colorspace fixture is unavailable")
        return 77

    temporary = None
    output = args.output
    if not output:
        temporary = tempfile.TemporaryDirectory(prefix="lightusd-colorspace-")
        output = temporary.name
    os.makedirs(output, exist_ok=True)

    cases = [
        ("core", ["-rtPreview"]),
    ]
    results = []
    for backend, backend_args in cases:
        image = os.path.join(output, f"colorspace-{backend}.png")
        command = [args.lusdrender, fixture, image, *backend_args,
                   "-w", "64", "-height", "64", "-autoframe",
                   "-samples", "1", "-threads", "1"]
        run = subprocess.run(command, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=60)
        if run.returncode != 0:
            print(run.stdout.decode(errors="replace"))
            print(f"FAIL: {backend} render exited {run.returncode}")
            return 1
        pixel = center_rgb(image)
        results.append({"backend": backend, "pixel": pixel, "image": image})

    # Texture colors are sampled rather than folded into shader constants.
    # Exercise both UsdUVTexture's sRGB/raw transfer switch and the higher
    # precedence `colorSpace` metadata on inputs:file. The latter carries the
    # OpenUSD wide-gamut scene spaces that sourceColorSpace itself cannot name.
    texture_fixture = os.path.join(args.repo_root, "tests", "usda",
                                   "colorspace-texture-render.usda")
    texture_asset = os.path.join(args.repo_root, "tests", "usda", "textures",
                                 "alpha-billboard-bird.png")
    texture_results = []
    materialx_texture_results = []
    if os.path.isfile(texture_fixture) and os.path.isfile(texture_asset):
        generated = os.path.join(output, "texture-fixtures")
        generated_textures = os.path.join(generated, "textures")
        os.makedirs(generated_textures, exist_ok=True)
        shutil.copy2(texture_asset,
                     os.path.join(generated_textures,
                                  "alpha-billboard-bird.png"))
        with open(texture_fixture, encoding="utf-8") as f:
            texture_source = f.read()

        variants = {
            "srgb": texture_source,
            "raw": texture_source.replace('sourceColorSpace = "sRGB"',
                                            'sourceColorSpace = "raw"'),
        }
        asset_opinion = (
            "asset inputs:file = @textures/alpha-billboard-bird.png@")
        for color_space in ("lin_rec709_scene", "g22_rec709_scene",
                            "lin_ap1_scene", "lin_ap0_scene"):
            variants[color_space] = texture_source.replace(
                asset_opinion,
                asset_opinion + f' (colorSpace = "{color_space}")')
        variants["studio_ap1"] = texture_source.replace(
            asset_opinion,
            asset_opinion + ' (colorSpace = "studio_ap1")')
        variants["studio_srgb"] = texture_source.replace(
            asset_opinion,
            asset_opinion + ' (colorSpace = "studio_srgb")')

        texture_images = {}
        for variant, source in variants.items():
            scene = os.path.join(generated, f"{variant}.usda")
            with open(scene, "w", encoding="utf-8") as f:
                f.write(source)
            for backend, backend_args in cases:
                image = os.path.join(output,
                                     f"texture-{backend}-{variant}.png")
                command = [args.lusdrender, scene, image, *backend_args,
                           "-w", "96", "-height", "96", "-autoframe",
                           "-samples", "1", "-threads", "1"]
                run = subprocess.run(command, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, timeout=60)
                if run.returncode != 0:
                    print(run.stdout.decode(errors="replace"))
                    print(f"FAIL: {backend} texture {variant} render exited "
                          f"{run.returncode}")
                    return 1
                texture_images[backend, variant] = image

        for backend, _ in cases:
            comparisons = {
                "srgb-vs-raw": compare_pixels(
                    texture_images[backend, "srgb"],
                    texture_images[backend, "raw"]),
                "linear-vs-g22": compare_pixels(
                    texture_images[backend, "lin_rec709_scene"],
                    texture_images[backend, "g22_rec709_scene"]),
                "rec709-vs-ap1": compare_pixels(
                    texture_images[backend, "lin_rec709_scene"],
                    texture_images[backend, "lin_ap1_scene"]),
                "ap1-vs-ap0": compare_pixels(
                    texture_images[backend, "lin_ap1_scene"],
                    texture_images[backend, "lin_ap0_scene"]),
                "builtin-ap1-vs-studio-ap1": compare_pixels(
                    texture_images[backend, "lin_ap1_scene"],
                    texture_images[backend, "studio_ap1"]),
                "builtin-srgb-vs-studio-srgb": compare_pixels(
                    texture_images[backend, "srgb"],
                    texture_images[backend, "studio_srgb"]),
            }
            texture_results.append({"backend": backend,
                                    "comparisons": comparisons})

        materialx_texture_fixture = os.path.join(
            args.repo_root, "tests", "usda",
            "colorspace-materialx-config-texture-render.usda")
        if os.path.isfile(materialx_texture_fixture):
            with open(materialx_texture_fixture, encoding="utf-8") as f:
                mtlx_texture_source = f.read()
            explicit_mtlx_texture = mtlx_texture_source.replace(
                asset_opinion,
                asset_opinion + ' (colorSpace = "lin_ap1_scene")')
            rec709_mtlx_texture = mtlx_texture_source.replace(
                'string config:mtlx:colorspace = "lin_ap1_scene"',
                'string config:mtlx:colorspace = "lin_rec709_scene"')
            materialx_texture_images = {}
            for variant, source in {
                    "configured-ap1": mtlx_texture_source,
                    "explicit-ap1": explicit_mtlx_texture,
                    "configured-rec709": rec709_mtlx_texture}.items():
                scene = os.path.join(generated, f"mtlx-{variant}.usda")
                with open(scene, "w", encoding="utf-8") as f:
                    f.write(source)
                for backend, backend_args in cases:
                    image = os.path.join(
                        output, f"materialx-texture-{backend}-{variant}.png")
                    command = [args.lusdrender, scene, image, *backend_args,
                               "-w", "96", "-height", "96", "-autoframe",
                               "-samples", "1", "-threads", "1"]
                    run = subprocess.run(
                        command, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, timeout=60)
                    if run.returncode != 0:
                        print(run.stdout.decode(errors="replace"))
                        print(f"FAIL: {backend} MaterialX texture {variant} "
                              f"render exited {run.returncode}")
                        return 1
                    materialx_texture_images[backend, variant] = image
            for backend, _ in cases:
                materialx_texture_results.append({
                    "backend": backend,
                    "comparisons": {
                        "config-vs-explicit-ap1": compare_pixels(
                            materialx_texture_images[
                                backend, "configured-ap1"],
                            materialx_texture_images[
                                backend, "explicit-ap1"]),
                        "ap1-vs-rec709-config": compare_pixels(
                            materialx_texture_images[
                                backend, "configured-ap1"],
                            materialx_texture_images[
                                backend, "configured-rec709"]),
                    },
                })

    # MaterialX document colorspace is a fallback for untagged color3/color4
    # values, including values reached through a NodeGraph. Compare the
    # configured form to an equivalent explicit property opinion at rendered
    # pixel level. A Rec.709-configured negative control ensures a no-op render
    # cannot make the parity comparison pass accidentally.
    materialx_fixture = os.path.join(
        args.repo_root, "tests", "usda",
        "colorspace-materialx-config-render.usda")
    materialx_results = []
    if os.path.isfile(materialx_fixture):
        with open(materialx_fixture, encoding="utf-8") as f:
            configured_source = f.read()
        value_opinion = "color3f inputs:value = (0.25, 0.5, 0.75)"
        explicit_source = configured_source.replace(
            value_opinion,
            value_opinion + ' (colorSpace = "lin_ap1_scene")')
        rec709_source = configured_source.replace(
            'string config:mtlx:colorspace = "lin_ap1_scene"',
            'string config:mtlx:colorspace = "lin_rec709_scene"')
        generated = os.path.join(output, "materialx-fixtures")
        os.makedirs(generated, exist_ok=True)
        materialx_images = {}
        for variant, source in {
                "configured-ap1": configured_source,
                "explicit-ap1": explicit_source,
                "configured-rec709": rec709_source}.items():
            scene = os.path.join(generated, f"{variant}.usda")
            with open(scene, "w", encoding="utf-8") as f:
                f.write(source)
            for backend, backend_args in cases:
                image = os.path.join(
                    output, f"materialx-{backend}-{variant}.png")
                command = [args.lusdrender, scene, image, *backend_args,
                           "-w", "64", "-height", "64", "-autoframe",
                           "-samples", "1", "-threads", "1"]
                run = subprocess.run(command, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, timeout=60)
                if run.returncode != 0:
                    print(run.stdout.decode(errors="replace"))
                    print(f"FAIL: {backend} MaterialX {variant} render "
                          f"exited {run.returncode}")
                    return 1
                materialx_images[backend, variant] = image
        for backend, _ in cases:
            materialx_results.append({
                "backend": backend,
                "configuredPixel": center_rgb(
                    materialx_images[backend, "configured-ap1"]),
                "comparisons": {
                    "config-vs-explicit-ap1": compare_pixels(
                        materialx_images[backend, "configured-ap1"],
                        materialx_images[backend, "explicit-ap1"]),
                    "ap1-vs-rec709-config": compare_pixels(
                        materialx_images[backend, "configured-ap1"],
                        materialx_images[backend, "configured-rec709"]),
                },
            })

    expected = [62, 117, 158]
    failures = []
    for result in results:
        delta = max(abs(a - b) for a, b in zip(result["pixel"], expected))
        result["expected"] = expected
        result["maxDelta"] = delta
        if delta > args.tolerance:
            failures.append(result["backend"])
    # Byte-level thresholds are deliberately well below the observed deltas;
    # they reject a bypassed transfer/gamut transform without depending on one
    # backend's different auto-frame footprint or lighting estimator.
    minimum_texture_delta = {
        "srgb-vs-raw": 20,
        "linear-vs-g22": 20,
        "rec709-vs-ap1": 4,
        "ap1-vs-ap0": 2,
    }
    for result in texture_results:
        for name, minimum in minimum_texture_delta.items():
            if name in result["comparisons"] and \
                    result["comparisons"][name]["maxDelta"] < minimum:
                failures.append(f'{result["backend"]}-texture-{name}')
        custom = result["comparisons"].get("builtin-ap1-vs-studio-ap1")
        if custom and custom["maxDelta"] > args.tolerance:
            failures.append(
                f'{result["backend"]}-texture-custom-definition-parity')
        custom_transfer = result["comparisons"].get(
            "builtin-srgb-vs-studio-srgb")
        if custom_transfer and custom_transfer["maxDelta"] > args.tolerance:
            failures.append(
                f'{result["backend"]}-texture-custom-transfer-parity')
    for result in materialx_results:
        materialx_expected = [104, 206, 219]
        result["expected"] = materialx_expected
        result["maxDelta"] = max(
            abs(a - b) for a, b in
            zip(result["configuredPixel"], materialx_expected))
        if result["maxDelta"] > args.tolerance:
            failures.append(
                f'{result["backend"]}-materialx-config-pixel')
        parity = result["comparisons"]["config-vs-explicit-ap1"]
        if parity["maxDelta"] > args.tolerance:
            failures.append(
                f'{result["backend"]}-materialx-config-explicit-parity')
        negative = result["comparisons"]["ap1-vs-rec709-config"]
        if negative["maxDelta"] < 4:
            failures.append(
                f'{result["backend"]}-materialx-config-negative-control')
    for result in materialx_texture_results:
        parity = result["comparisons"]["config-vs-explicit-ap1"]
        if parity["maxDelta"] > args.tolerance:
            failures.append(
                f'{result["backend"]}-materialx-texture-config-parity')
        negative = result["comparisons"]["ap1-vs-rec709-config"]
        if negative["maxDelta"] < 4:
            failures.append(
                f'{result["backend"]}-materialx-texture-negative-control')
    report = {"pass": not failures, "source": "srgb_rec709_scene",
              "working": "lin_ap1_scene", "tolerance": args.tolerance,
              "failures": failures,
              "results": results, "textureResults": texture_results,
              "materialXResults": materialx_results,
              "materialXTextureResults": materialx_texture_results}
    print(json.dumps(report, indent=2))
    if args.output:
        with open(os.path.join(output, "colorspace-regression.json"), "w") as f:
            json.dump(report, f, indent=2)
            f.write("\n")
    if temporary:
        temporary.cleanup()
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())

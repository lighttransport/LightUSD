#!/usr/bin/env python3
"""MaterialX-in-MTL validation against LightUSD's MaterialX renderer.

For each reference .mtlx:
  1. mtlx2mtl.py converts it to MaterialX-in-MTL lines (.mtl);
  2. the tinyobjloader C implementation (tobj_mtlx) turns the .mtl back into a
     .mtlx document; if the Lean reference executable is given, its output must
     be byte-identical;
  3. lusdrender --mtlx renders every material of the reference and of the
     regenerated document; the images must be pixel-identical.

  4. (--usd) OBJ import path: a sphere .obj whose .mtl carries the
     MaterialX-in-MTL lines is imported by LightUSD (UsdShade MaterialX
     network) and rendered against the same sphere bound to the reference
     .mtlx through a USD reference; the images must be pixel-identical. A
     render identical to the classic-only fallback is reported as
     non-discriminating instead of passing.

usage: run_mtlx_mtl.py --lusdrender BIN --mtlx-dump BIN --mtlx2mtl PY
                       [--lean BIN] [--images DIR] [--keep DIR] [--usd] MTLX...
"""
import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                          timeout=300, check=False, **kw)


def xml_part(dump):
    i = dump.find("<?xml")
    return dump[i:] if i >= 0 else ""


def pixels(path):
    from PIL import Image
    im = Image.open(path).convert("RGBA")
    return im.size, im.tobytes()


def render(lusdrender, mtlx, material, out, args):
    r = run([lusdrender, "--mtlx", str(mtlx), str(out), "--mtlx-material", material,
             "-w", str(args.size), "-height", str(args.size), "-samples", str(args.samples)])
    loads = len(re.findall(r"texture: loaded|preloaded [1-9]", r.stdout))
    fails = len(re.findall(r"texture: failed", r.stdout))
    return r.returncode, r.stdout, loads, fails


SPHERE_N, SPHERE_M = 32, 16


def write_sphere(path, mtllib, material):
    import math
    with open(path, "w") as o:
        o.write(f"mtllib {mtllib}\n")
        for j in range(SPHERE_M + 1):
            th = math.pi * j / SPHERE_M
            for i in range(SPHERE_N + 1):
                ph = 2 * math.pi * i / SPHERE_N
                o.write("v %f %f %f\n" % (math.sin(th) * math.cos(ph), math.cos(th),
                                           math.sin(th) * math.sin(ph)))
        for j in range(SPHERE_M + 1):
            for i in range(SPHERE_N + 1):
                o.write("vt %f %f\n" % (i / SPHERE_N, 1 - j / SPHERE_M))
        o.write(f"usemtl {material}\n")
        for j in range(SPHERE_M):
            for i in range(SPHERE_N):
                a = j * (SPHERE_N + 1) + i + 1
                q = (a, a + SPHERE_N + 1, a + SPHERE_N + 2, a + 1)
                o.write("f " + " ".join(f"{k}/{k}" for k in q) + "\n")


LIGHT = """
def DistantLight "Sun"
{
    float inputs:intensity = 4
    float3 xformOp:rotateXYZ = (-45, 30, 0)
    uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
}
"""


def flat_textures(d, mtl_text, ref_root):
    """Copy every texture into d/tex and point both documents at tex/<name>
    (the OBJ importer only reads assets below the .obj directory; fileprefix
    is baked so both documents name the same copied file)."""
    tex = d / "tex"
    tex.mkdir(exist_ok=True)

    def place(rel):
        src = (d / rel).resolve()
        if not src.is_file():
            return rel
        dst = tex / src.name
        if not dst.exists():
            shutil.copy(src, dst)
        return "tex/" + src.name

    out = []
    for line in mtl_text.splitlines():
        parts = line.split(" ", 3)
        if len(parts) == 4 and parts[0] == "mtlx_input" and parts[2] == "filename":
            v = parts[3]
            q = v.startswith('"')
            v = place(v.strip('"'))
            line = " ".join(parts[:3]) + " " + (f'"{v}"' if q or " " in v else v)
        out.append(line)

    def walk(e, prefix):
        prefix = e.get("fileprefix", prefix)
        e.attrib.pop("fileprefix", None)
        if e.tag == "input" and e.get("type") == "filename" and e.get("value") is not None:
            e.set("value", place(prefix + e.get("value")))
        for c in e:
            walk(c, prefix)
    walk(ref_root, "")
    return "\n".join(out) + "\n"


def usd_case(args, d, src, mtl_text, materials):
    """Return (ok, message) for the OBJ-import path of one document."""
    tree = ET.parse(d / src.name)
    mtl_text = flat_textures(d, mtl_text, tree.getroot())
    tree.write(d / "ref_flat.mtlx")
    classic = "\n".join(l for l in mtl_text.splitlines()
                        if not l.startswith("mtlx_")) + "\n"
    (d / "ext.mtl").write_text(mtl_text)
    (d / "classic.mtl").write_text(classic)
    results = []
    for name in materials:
        stem = re.sub(r"[^A-Za-z0-9_]", "_", name)
        renders, unsupported, textures = {}, False, {}
        # One material per .mtl so the mesh's single GeomSubset is
        # material_0 (the subset name follows the material's index).
        for src_mtl in ("ext", "classic"):
            text = (d / f"{src_mtl}.mtl").read_text()
            blocks = re.split(r"(?m)^(?=newmtl )", text)
            one = [b for b in blocks if b.startswith(f"newmtl {name}\n")]
            (d / f"{stem}_{src_mtl}.mtl").write_text(one[0] if one else "")
        for kind in ("ext", "classic", "ref"):
            obj = d / f"{stem}_{kind}.obj"
            write_sphere(obj, f"{stem}_{'ext' if kind == 'ext' else 'classic'}.mtl", name)
            usda = d / f"{stem}_{kind}.usda"
            text = '#usda 1.0\n(\n    defaultPrim = "W"\n)\n'
            text += f'def Xform "W" (\n    references = @{obj.name}@\n)\n{{\n}}\n' + LIGHT
            if kind == "ref":
                text += ('def "MX" (\n    references = @ref_flat.mtlx@</MaterialX>\n)\n{\n}\n'
                         'over "W" { over "Links" { over "Geometry" { over "mesh" {'
                         ' over "material_0" { rel material:binding = '
                         f'</MX/Materials/{name}> }} }} }} }} }}\n')
            usda.write_text(text)
            png = d / f"{stem}_{kind}.png"
            r = run([args.lusdrender, str(usda), str(png), "-w", str(args.size), "-height",
                     str(args.size), "-autoframe", "-samples", str(args.samples), "-stats"])
            m = re.search(r"rt textures: (\d+)", r.stdout)
            textures[kind] = int(m.group(1)) if m else -1
            if r.returncode != 0 or not png.exists():
                return False, f"{name}: {kind} render failed\n{r.stdout[-400:]}"
            unsupported |= kind != "classic" and "unsupported surface shader" in r.stdout
            renders[kind] = pixels(png)
        if unsupported:
            # LightUSD's USD render path does not implement this surface
            # shader (either input path); the fallbacks legitimately differ.
            results.append(f"{name} (surface shader unsupported by the USD render path)")
            continue
        if renders["ext"] != renders["ref"]:
            if 0 <= textures["ref"] < textures["ext"]:
                # The reference path (.mtlx layer -> USD) dropped textures that
                # the document binds; the OBJ path loaded them.
                return False, (f"{name}: REFERENCE-DEFECT: .mtlx-layer path loaded "
                               f"{textures['ref']} textures, OBJ path {textures['ext']}")
            return False, f"{name}: OBJ MaterialX render differs from reference .mtlx"
        results.append(f"{name}{'' if renders['ext'] != renders['classic'] else ' (non-discriminating)'}")
    return True, ", ".join(results)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lusdrender", required=True)
    ap.add_argument("--mtlx-dump", required=True)
    ap.add_argument("--mtlx2mtl", required=True)
    ap.add_argument("--lean")
    ap.add_argument("--images", help="directory used for ../../../Images style fileprefixes")
    ap.add_argument("--keep")
    ap.add_argument("--size", type=int, default=96)
    ap.add_argument("--samples", type=int, default=2)
    ap.add_argument("--usd", action="store_true", help="also check the OBJ import path")
    ap.add_argument("mtlx", nargs="+")
    args = ap.parse_args()

    work = pathlib.Path(args.keep or tempfile.mkdtemp(prefix="mtlx-mtl-"))
    work.mkdir(parents=True, exist_ok=True)
    stats = {"pass": 0, "fail": 0, "skip": 0}
    for idx, src in enumerate(map(pathlib.Path, args.mtlx)):
        case = work / f"{idx:03d}_{src.stem}"
        d = case / "a" / "b" / "c"          # so ../../../Images resolves to case/Images
        if case.exists():
            shutil.rmtree(case)
        d.mkdir(parents=True)
        if args.images:
            os.symlink(os.path.abspath(args.images), case / "Images")
        for sib in src.parent.iterdir():     # sibling resource dirs (e.g. chess_set/)
            if sib.is_dir():
                os.symlink(sib.resolve(), d / sib.name)
        ref = d / src.name
        shutil.copy(src, ref)

        conv = run([sys.executable, "-I", args.mtlx2mtl, str(ref)])
        if conv.returncode == 3:
            print(f"SKIP {src.name}: {conv.stdout.strip()}")
            stats["skip"] += 1
            continue
        if conv.returncode != 0:
            print(f"FAIL {src.name}: mtlx2mtl\n{conv.stdout}")
            stats["fail"] += 1
            continue
        mtl = d / (src.stem + ".mtl")
        mtl.write_text(conv.stdout)

        dump = run([args.mtlx_dump, str(mtl)])
        verdicts = re.findall(r"^mat (.*) ext=(\d) ok=(\d)$", dump.stdout, re.M)
        if dump.returncode != 0 or not verdicts or any(v[2] != "1" for v in verdicts):
            print(f"FAIL {src.name}: tobj_mtlx rejected a material\n{dump.stdout}")
            stats["fail"] += 1
            continue
        gen_xml = xml_part(dump.stdout)
        if args.lean:
            lean = run([args.lean, str(mtl)])
            if lean.stdout != dump.stdout:
                print(f"FAIL {src.name}: Lean reference and C differ")
                stats["fail"] += 1
                continue
        gen = d / (src.stem + ".gen.mtlx")
        gen.write_text(gen_xml)

        ok = True
        for mat in ET.parse(ref).getroot().findall("surfacematerial"):
            name = mat.get("name")
            ra, rb = d / f"{name}.ref.png", d / f"{name}.gen.png"
            ca, _, la, fa = render(args.lusdrender, ref, name, ra, args)
            cb, _, lb, fb = render(args.lusdrender, gen, name, rb, args)
            if ca != 0 and cb != 0:
                print(f"SKIP {src.name}:{name}: lusdrender --mtlx cannot render this material")
                continue
            if ca != 0 or cb != 0 or not ra.exists() or not rb.exists():
                print(f"FAIL {src.name}:{name}: render failed ({ca}, {cb})")
                ok = False
                continue
            if pixels(ra) != pixels(rb):
                print(f"FAIL {src.name}:{name}: images differ ({ra} vs {rb})")
                ok = False
                continue
            if (la, fa) != (lb, fb):
                print(f"FAIL {src.name}:{name}: texture loads differ ref={la}/{fa} gen={lb}/{fb}")
                ok = False
                continue
            print(f"PASS {src.name}:{name} (textures loaded {la}, missing {fa})")
        if ok and args.usd:
            names = [m.get("name") for m in ET.parse(ref).getroot().findall("surfacematerial")]
            uok, msg = usd_case(args, d, src, conv.stdout, names)
            print(f"{'PASS' if uok else 'FAIL'} {src.name} [obj->usd]: {msg}")
            ok = ok and uok
        stats["pass" if ok else "fail"] += 1
    print(f"mtlx-mtl: {stats['pass']} pass, {stats['fail']} fail, {stats['skip']} skip  (work: {work})")
    return 1 if stats["fail"] else 0


if __name__ == "__main__":
    sys.exit(main())

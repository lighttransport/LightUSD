"""Command line: python -m miniusd {cat,convert,info} ..."""

import argparse
import sys

from . import open as open_usd


def _tree(layer):
    lines = []

    def rec(prim, depth):
        lines.append("%s%s %s%s" % ("  " * depth, prim.specifier or "", prim.type_name + " " if prim.type_name else "",
                                    prim.path))
        for c in prim.children.values():
            rec(c, depth + 1)
    for c in layer.root.children.values():
        rec(c, 0)
    return lines


def main(argv=None):
    ap = argparse.ArgumentParser(prog="miniusd", description="Mini USD (pure python) tools")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("cat", help="print a USD file as USDA, or write it with -o")
    p.add_argument("input")
    p.add_argument("-o", "--output")
    p = sub.add_parser("convert", help="convert between .usda/.usdc/.usd/.usdz")
    p.add_argument("input")
    p.add_argument("output")
    p = sub.add_parser("info", help="print prim tree and stats")
    p.add_argument("input")
    args = ap.parse_args(argv)

    layer = open_usd(args.input)
    for w in layer.warnings:
        print("warning: " + w, file=sys.stderr)
    if args.cmd == "cat" and not args.output:
        sys.stdout.write(layer.to_usda())
    elif args.cmd in ("cat", "convert"):
        layer.save(args.output or args.input)
    else:
        prims = list(layer.traverse())
        nprops = sum(len(p.properties) for p in prims)
        print("\n".join(_tree(layer)))
        print("-- %d prims, %d properties, %d assets" % (len(prims), nprops, len(layer.assets)))
        for k in ("defaultPrim", "upAxis", "metersPerUnit"):
            if k in layer.metadata:
                print("%s: %s" % (k, layer.metadata[k]))
    return 0


if __name__ == "__main__":
    sys.exit(main())

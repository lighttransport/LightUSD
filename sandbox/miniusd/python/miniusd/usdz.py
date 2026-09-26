"""USDZ package read/write: uncompressed zip, every file's data 64-byte aligned,
root layer first. Asset files (textures etc.) are stored as-is."""

import io
import os
import zipfile

_USD_EXTS = (".usd", ".usda", ".usdc")


def _aligned_info(name, offset):
    zi = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    zi.compress_type = zipfile.ZIP_STORED
    zi.create_system = 0
    header = 30 + len(name.encode("utf-8"))
    pad = (-(offset + header)) % 64
    if 0 < pad < 4:
        pad += 64
    if pad:
        # extra field: id 0x1986 (padding), size, zero bytes
        zi.extra = (0x1986).to_bytes(2, "little") + (pad - 4).to_bytes(2, "little") + bytes(pad - 4)
    return zi


def pack_usdz(files):
    """files: list of (archive name, bytes); the first must be the root layer."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as zf:
        for name, data in files:
            zf.writestr(_aligned_info(name, buf.tell()), data)
    return buf.getvalue()


def write_usdz(layer, assets=None, root_format="usdc", root_name=None):
    if root_format == "usda":
        root = layer.to_usda().encode("utf-8")
    else:
        root = layer.to_usdc()
    files = [(root_name or ("scene." + root_format), root)]
    all_assets = dict(layer.assets)
    all_assets.update(assets or {})
    for name, data in all_assets.items():
        if not isinstance(data, (bytes, bytearray)):
            with open(os.fspath(data), "rb") as f:
                data = f.read()
        files.append((name.replace("\\", "/").lstrip("/"), bytes(data)))
    return pack_usdz(files)


def read_usdz(data):
    """Returns (root layer name, root layer bytes, {asset name: bytes})."""
    zf = zipfile.ZipFile(io.BytesIO(data))
    root_name, root, assets = None, None, {}
    for zi in zf.infolist():
        if zi.is_dir():
            continue
        b = zf.read(zi)
        if root is None and zi.filename.lower().endswith(_USD_EXTS):
            root_name, root = zi.filename, b
        else:
            assets[zi.filename] = b
    if root is None:
        raise ValueError("USDZ contains no USD layer")
    return root_name, root, assets

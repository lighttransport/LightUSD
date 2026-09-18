"""HTTP base64 fallback client for UE MCP asset exchange."""

from __future__ import annotations

import base64
import hashlib
import io
import json
from pathlib import Path
from urllib import request
import zipfile


def upload_file(url: str, filename: str, token: str = "") -> dict:
    path = Path(filename)
    payload = path.read_bytes()
    digest = hashlib.sha256(payload).hexdigest()
    body = json.dumps({
        "name": path.name,
        "data": base64.b64encode(payload).decode("ascii"),
        "sha256": digest,
    }).encode("utf-8")
    headers = {"Content-Type": "application/json"}
    if token:
        headers["X-LightUSD-Bridge-Token"] = token
    req = request.Request(url.rstrip("/") + "/v1/upload", body, headers, method="POST")
    with request.urlopen(req, timeout=30) as response:
        return json.loads(response.read().decode("utf-8"))


def download_file(url: str, asset_id: str, filename: str, token: str = "") -> dict:
    headers = {"Accept": "application/json"}
    if token:
        headers["X-LightUSD-Bridge-Token"] = token
    req = request.Request(url.rstrip("/") + "/v1/download/" + asset_id, headers=headers)
    with request.urlopen(req, timeout=30) as response:
        metadata = json.loads(response.read().decode("utf-8"))
    payload = base64.b64decode(metadata.pop("data"), validate=True)
    if hashlib.sha256(payload).hexdigest() != metadata["sha256"]:
        raise ValueError("bridge asset checksum mismatch")
    destination = Path(filename)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".part")
    temporary.write_bytes(payload)
    temporary.replace(destination)
    return metadata


def download_bundle(url: str, asset_id: str, destination: str, token: str = "",
                    max_bytes: int = 2 * 1024 * 1024 * 1024) -> dict:
    """Download and safely extract a checksummed LightUSD dependency bundle."""
    headers = {"Accept": "application/json"}
    if token:
        headers["X-LightUSD-Bridge-Token"] = token
    req = request.Request(url.rstrip("/") + "/v1/download/" + asset_id,
                          headers=headers)
    with request.urlopen(req, timeout=30) as response:
        metadata = json.loads(response.read().decode("utf-8"))
    payload = base64.b64decode(metadata.pop("data"), validate=True)
    if hashlib.sha256(payload).hexdigest() != metadata["sha256"]:
        raise ValueError("bridge bundle checksum mismatch")
    root = Path(destination)
    root.mkdir(parents=True, exist_ok=True)
    total = 0
    with zipfile.ZipFile(io.BytesIO(payload), "r") as archive:
        manifest = json.loads(archive.read("lightusd-bundle.json"))
        if manifest.get("format") != "lightusd-asset-bundle-v1":
            raise ValueError("unsupported LightUSD asset bundle")
        for entry in manifest.get("files", []):
            relative = str(entry["path"]).replace("\\", "/")
            parts = [part for part in relative.split("/") if part not in ("", ".")]
            if (not parts or relative.startswith("/") or ".." in parts or
                    ":" in parts[0]):
                raise ValueError("unsafe path in LightUSD asset bundle")
            data = archive.read("files/" + "/".join(parts))
            total += len(data)
            if total > max_bytes:
                raise ValueError("expanded LightUSD asset bundle exceeds size limit")
            if (len(data) != int(entry["size"]) or
                    hashlib.sha256(data).hexdigest() != entry["sha256"]):
                raise ValueError("LightUSD asset bundle entry failed verification")
            target = root.joinpath(*parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            temporary = target.with_name(target.name + ".part")
            temporary.write_bytes(data)
            temporary.replace(target)
    metadata["bundle"] = manifest
    return metadata

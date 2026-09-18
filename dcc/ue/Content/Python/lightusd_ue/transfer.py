"""Streaming HTTP fallback client for UE MCP asset exchange."""

from __future__ import annotations

import hashlib
import http.client
import json
import tempfile
from pathlib import Path
from urllib import parse
import zipfile


_CHUNK_SIZE = 1024 * 1024
_MANIFEST = "lightusd-bundle.json"


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(_CHUNK_SIZE), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _connection(url: str):
    parsed = parse.urlparse(url)
    if parsed.scheme not in ("http", ""):
        raise ValueError("LightUSD asset bridge requires HTTP")
    return parsed, http.client.HTTPConnection(
        parsed.hostname, parsed.port or 80, timeout=30)


def _error(payload: bytes, status: int) -> str:
    try:
        return json.loads(payload.decode("utf-8")).get(
            "error", "bridge HTTP error " + str(status))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return "bridge HTTP error " + str(status)


def upload_file(url: str, filename: str, token: str = "") -> dict:
    """Stream a file to the content-addressed bridge."""
    path = Path(filename)
    digest = _sha256(path)
    target = url.rstrip("/") + "/v1/upload-raw?" + parse.urlencode(
        {"name": path.name})
    parsed, connection = _connection(target)
    headers = {
        "Content-Type": "application/octet-stream",
        "Content-Length": str(path.stat().st_size),
        "X-LightUSD-SHA256": digest,
    }
    if token:
        headers["X-LightUSD-Bridge-Token"] = token
    with path.open("rb") as stream:
        connection.request(
            "PUT", parsed.path + "?" + parsed.query, stream, headers)
        response = connection.getresponse()
        payload = response.read()
    if response.status >= 300:
        raise ValueError(_error(payload, response.status))
    return json.loads(payload.decode("utf-8"))


def download_file(url: str, asset_id: str, filename: str,
                  token: str = "") -> dict:
    """Stream, verify, and atomically store a bridge asset."""
    target = url.rstrip("/") + "/v1/download-raw/" + asset_id
    parsed, connection = _connection(target)
    headers = {}
    if token:
        headers["X-LightUSD-Bridge-Token"] = token
    connection.request("GET", parsed.path, headers=headers)
    response = connection.getresponse()
    if response.status >= 300:
        raise ValueError(_error(response.read(), response.status))
    destination = Path(filename)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".part")
    digest = hashlib.sha256()
    size = 0
    try:
        with temporary.open("wb") as output:
            while True:
                chunk = response.read(_CHUNK_SIZE)
                if not chunk:
                    break
                output.write(chunk)
                digest.update(chunk)
                size += len(chunk)
        expected = response.getheader("X-LightUSD-SHA256", "")
        declared_size = int(response.getheader("Content-Length", "-1"))
        if expected != asset_id or digest.hexdigest() != expected:
            raise ValueError("bridge asset checksum mismatch")
        if size != declared_size:
            raise ValueError("bridge asset size mismatch")
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)
    return {
        "id": asset_id,
        "name": response.getheader("X-LightUSD-Name", destination.name),
        "size": size,
        "sha256": expected,
    }


def _bundle_path(value: str) -> str:
    relative = str(value).replace("\\", "/")
    parts = [part for part in relative.split("/") if part not in ("", ".")]
    if (not parts or relative.startswith("/") or ".." in parts or
            ":" in parts[0]):
        raise ValueError("unsafe path in LightUSD asset bundle")
    return "/".join(parts)


def _extract_bundle(filename: Path, destination: Path,
                    max_bytes: int) -> dict:
    destination.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(filename, "r") as archive:
        manifest_info = archive.getinfo(_MANIFEST)
        if manifest_info.file_size > min(max_bytes, 16 * 1024 * 1024):
            raise ValueError("LightUSD bundle manifest exceeds size limit")
        manifest = json.loads(archive.read(manifest_info))
        if manifest.get("format") != "lightusd-asset-bundle-v1":
            raise ValueError("unsupported LightUSD asset bundle")
        validated = []
        portable_names = set()
        declared_total = 0
        for entry in manifest.get("files", []):
            relative = _bundle_path(entry["path"])
            portable_name = relative.casefold()
            if portable_name in portable_names:
                raise ValueError("portable LightUSD bundle path collision")
            portable_names.add(portable_name)
            declared_size = int(entry["size"])
            if declared_size < 0:
                raise ValueError("negative LightUSD bundle entry size")
            declared_total += declared_size
            if declared_total > max_bytes:
                raise ValueError(
                    "expanded LightUSD asset bundle exceeds size limit")
            if archive.getinfo("files/" + relative).file_size != declared_size:
                raise ValueError("LightUSD bundle entry size mismatch")
            validated.append((entry, relative))

        total = 0
        with tempfile.TemporaryDirectory(
                dir=destination, prefix="extract-") as staging_value:
            staging = Path(staging_value)
            for entry, relative in validated:
                output_path = staging.joinpath(*relative.split("/"))
                output_path.parent.mkdir(parents=True, exist_ok=True)
                digest = hashlib.sha256()
                size = 0
                with archive.open("files/" + relative) as source, \
                        output_path.open("wb") as output:
                    while True:
                        chunk = source.read(_CHUNK_SIZE)
                        if not chunk:
                            break
                        output.write(chunk)
                        digest.update(chunk)
                        size += len(chunk)
                        total += len(chunk)
                        if total > max_bytes:
                            raise ValueError(
                                "expanded LightUSD asset bundle exceeds size limit")
                if (size != int(entry["size"]) or
                        digest.hexdigest() != entry["sha256"]):
                    raise ValueError(
                        "LightUSD asset bundle entry failed verification")
            for _, relative in validated:
                source = staging.joinpath(*relative.split("/"))
                target = destination.joinpath(*relative.split("/"))
                target.parent.mkdir(parents=True, exist_ok=True)
                source.replace(target)
    return manifest


def download_bundle(url: str, asset_id: str, destination: str, token: str = "",
                    max_bytes: int = 2 * 1024 * 1024 * 1024) -> dict:
    """Stream, verify, and safely extract a dependency bundle."""
    root = Path(destination)
    root.mkdir(parents=True, exist_ok=True)
    bundle_path = None
    try:
        with tempfile.NamedTemporaryFile(
                dir=root, prefix="bundle-", suffix=".part",
                delete=False) as temporary:
            bundle_path = Path(temporary.name)
        metadata = download_file(url, asset_id, str(bundle_path), token)
        metadata["bundle"] = _extract_bundle(bundle_path, root, max_bytes)
        return metadata
    finally:
        if bundle_path is not None:
            bundle_path.unlink(missing_ok=True)

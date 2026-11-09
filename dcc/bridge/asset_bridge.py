"""Dependency-free HTTP/WebSocket base64 asset transfer bridge.

The bridge is deliberately not an MCP server.  It transports USD and related
assets when an MCP client cannot move files between the Linux and Windows
machines.  HTTP JSON is the primary path; the WebSocket endpoint uses the same
messages and is useful for clients that already maintain a socket connection.
"""

from __future__ import annotations

import argparse
import base64
import glob
import hashlib
import html
import http.client
import http.server
import io
import json
import os
import re
import secrets
import socket
import socketserver
import struct
import tempfile
import threading
import urllib.parse
import zipfile
from pathlib import Path
from typing import Any


_ID_RE = re.compile(r"^[0-9a-f]{64}$")
_NAME_RE = re.compile(r"[^A-Za-z0-9_.-]+")
_BUNDLE_MANIFEST = "lightusd-bundle.json"
_USD_ASSET_RE = re.compile(r"@((?:\\@|[^@])+)@")
_MTLX_FILE_RE = re.compile(
    r"(?:file|filename|sourceuri|href)\s*=\s*(['\"])(.*?)\1", re.IGNORECASE)
_TEXT_DEPENDENCY_SUFFIXES = {".usd", ".usda", ".usdc", ".mtlx"}


class BridgeError(ValueError):
    """A client-visible transfer error."""


def _dependency_text(data: bytes, suffix: str, label: str) -> str:
    """Return inspectable text, decoding Crate through LightUSD when needed."""
    is_binary_usd = data.startswith(b"PXR-USDC") or b"\x00" in data[:4096]
    if not is_binary_usd:
        return data.decode("utf-8", errors="replace")
    if suffix not in {".usd", ".usdc"}:
        return ""
    try:
        import lightusd
        return lightusd.load_bytes(data, format="usdc").export_usda()
    except (ImportError, RuntimeError, ValueError) as exc:
        raise BridgeError("cannot inspect binary USD dependencies: " + label) from exc


def _dependency_references(data: bytes, suffix: str, label: str) -> list[str]:
    text = _dependency_text(data, suffix, label)
    references = [match.replace("\\@", "@")
                  for match in _USD_ASSET_RE.findall(text)]
    if suffix == ".mtlx":
        references.extend(html.unescape(match[1])
                          for match in _MTLX_FILE_RE.findall(text))
    return references


def _usdz_external_references(source: Path) -> list[str]:
    """Inspect USDZ members and return references not stored in the package."""
    try:
        with zipfile.ZipFile(source, "r") as archive:
            names = {_bundle_path(name) for name in archive.namelist()
                     if not name.endswith("/")}
            references = []
            for name in sorted(names):
                suffix = Path(name).suffix.lower()
                if suffix not in _TEXT_DEPENDENCY_SUFFIXES:
                    continue
                for reference in _dependency_references(
                        archive.read(name), suffix, f"{source}[{name}]"):
                    outer = reference.split("[", 1)[0]
                    if not outer or "://" in outer or outer.startswith("/"):
                        continue
                    package_relative = str(
                        Path(name).parent.joinpath(outer)).replace("\\", "/")
                    package_relative = os.path.normpath(package_relative).replace("\\", "/")
                    if package_relative not in names and outer not in names:
                        references.append(package_relative)
            return references
    except zipfile.BadZipFile as exc:
        raise BridgeError("invalid USDZ package: " + str(source)) from exc


class BridgeStore:
    """Content-addressed file store with size and filename checks."""

    def __init__(self, root: str | os.PathLike[str], max_bytes: int = 512 * 1024 * 1024):
        self.root = Path(root).resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.max_bytes = int(max_bytes)
        self._lock = threading.Lock()

    @staticmethod
    def _safe_name(name: str) -> str:
        name = _NAME_RE.sub("_", Path(str(name)).name).strip("._")
        if not name:
            raise BridgeError("asset name is empty")
        return name[:180]

    def put(self, name: str, payload: bytes, sha256: str | None = None) -> dict[str, Any]:
        if len(payload) > self.max_bytes:
            raise BridgeError("asset exceeds bridge size limit")
        digest = hashlib.sha256(payload).hexdigest()
        if sha256 and sha256.lower() != digest:
            raise BridgeError("sha256 does not match asset data")
        safe_name = self._safe_name(name)
        asset_dir = self.root / digest
        path = asset_dir / safe_name
        with self._lock:
            asset_dir.mkdir(exist_ok=True)
            if not path.exists():
                temp = asset_dir / (safe_name + ".part")
                temp.write_bytes(payload)
                temp.replace(path)
        return {"id": digest, "name": safe_name, "size": len(payload), "sha256": digest}

    def put_stream(self, name: str, stream: Any, length: int,
                   sha256: str | None = None) -> dict[str, Any]:
        """Store an exact-length stream without buffering the asset in RAM."""
        if length < 0 or length > self.max_bytes:
            raise BridgeError("asset exceeds bridge size limit")
        safe_name = self._safe_name(name)
        digest_state = hashlib.sha256()
        remaining = length
        temporary_path: Path | None = None
        try:
            with tempfile.NamedTemporaryFile(
                    dir=self.root, prefix="upload-", suffix=".part",
                    delete=False) as temporary:
                temporary_path = Path(temporary.name)
                while remaining:
                    chunk = stream.read(min(1024 * 1024, remaining))
                    if not chunk:
                        raise BridgeError("stream ended before Content-Length")
                    temporary.write(chunk)
                    digest_state.update(chunk)
                    remaining -= len(chunk)
            digest = digest_state.hexdigest()
            if sha256 and sha256.lower() != digest:
                raise BridgeError("sha256 does not match asset data")
            asset_dir = self.root / digest
            destination = asset_dir / safe_name
            with self._lock:
                asset_dir.mkdir(exist_ok=True)
                if destination.exists():
                    temporary_path.unlink()
                else:
                    temporary_path.replace(destination)
            temporary_path = None
            return {"id": digest, "name": safe_name, "size": length,
                    "sha256": digest}
        finally:
            if temporary_path is not None:
                temporary_path.unlink(missing_ok=True)

    def get_path(self, asset_id: str) -> tuple[dict[str, Any], Path]:
        """Resolve and verify an asset without loading it into memory."""
        if not _ID_RE.match(asset_id):
            raise BridgeError("invalid asset id")
        asset_dir = self.root / asset_id
        candidates = [p for p in asset_dir.iterdir()
                      if p.is_file() and not p.name.endswith(".part")] \
            if asset_dir.is_dir() else []
        if len(candidates) != 1:
            raise BridgeError("asset not found")
        path = candidates[0]
        digest_state = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest_state.update(chunk)
        actual = digest_state.hexdigest()
        if actual != asset_id:
            raise BridgeError("stored asset checksum mismatch")
        size = path.stat().st_size
        return {"id": asset_id, "name": path.name, "size": size,
                "sha256": actual}, path

    def get(self, asset_id: str) -> tuple[dict[str, Any], bytes]:
        meta, path = self.get_path(asset_id)
        payload = path.read_bytes()
        return meta, payload


def _json_response(handler: http.server.BaseHTTPRequestHandler, status: int, value: Any) -> None:
    data = json.dumps(value, separators=(",", ":")).encode("utf-8")
    handler.send_response(status)
    handler.send_header("Content-Type", "application/json")
    handler.send_header("Content-Length", str(len(data)))
    handler.end_headers()
    handler.wfile.write(data)


class _Handler(http.server.BaseHTTPRequestHandler):
    server: "BridgeHTTPServer"

    def log_message(self, fmt: str, *args: Any) -> None:
        if self.server.verbose:
            super().log_message(fmt, *args)

    def _authorized(self) -> bool:
        expected = self.server.token
        return not expected or secrets.compare_digest(
            self.headers.get("X-LightUSD-Bridge-Token", ""), expected)

    def _read_json(self) -> dict[str, Any]:
        length = int(self.headers.get("Content-Length", "-1"))
        if length < 0 or length > self.server.max_request_bytes:
            raise BridgeError("invalid or oversized request")
        try:
            value = json.loads(self.rfile.read(length))
        except (json.JSONDecodeError, UnicodeDecodeError) as exc:
            raise BridgeError("request is not valid JSON") from exc
        if not isinstance(value, dict):
            raise BridgeError("request must be a JSON object")
        return value

    def do_GET(self) -> None:
        if not self._authorized():
            _json_response(self, 401, {"error": "unauthorized"})
            return
        if self.path == "/v1/ws" and self.headers.get("Upgrade", "").lower() == "websocket":
            _websocket_session(self, self.server)
            return
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/health":
            _json_response(self, 200, {"ok": True, "protocol": "lightusd-asset-bridge-v1"})
            return
        prefix = "/v1/download/"
        if parsed.path.startswith(prefix):
            try:
                meta, payload = self.server.store.get(parsed.path[len(prefix):])
                meta["data"] = base64.b64encode(payload).decode("ascii")
                _json_response(self, 200, meta)
            except BridgeError as exc:
                _json_response(self, 404, {"error": str(exc)})
            return
        raw_prefix = "/v1/download-raw/"
        if parsed.path.startswith(raw_prefix):
            try:
                meta, path = self.server.store.get_path(
                    parsed.path[len(raw_prefix):])
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(meta["size"]))
                self.send_header("X-LightUSD-Name", meta["name"])
                self.send_header("X-LightUSD-SHA256", meta["sha256"])
                self.end_headers()
                with path.open("rb") as stream:
                    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                        self.wfile.write(chunk)
            except BridgeError as exc:
                _json_response(self, 404, {"error": str(exc)})
            return
        _json_response(self, 404, {"error": "not found"})

    def do_PUT(self) -> None:
        if not self._authorized():
            _json_response(self, 401, {"error": "unauthorized"})
            return
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path != "/v1/upload-raw":
            _json_response(self, 404, {"error": "not found"})
            return
        try:
            length = int(self.headers.get("Content-Length", "-1"))
            query = urllib.parse.parse_qs(parsed.query)
            result = self.server.store.put_stream(
                query.get("name", ["asset.usda"])[0], self.rfile, length,
                self.headers.get("X-LightUSD-SHA256"))
            _json_response(self, 201, result)
        except (BridgeError, ValueError) as exc:
            _json_response(self, 400, {"error": str(exc)})

    def do_POST(self) -> None:
        if not self._authorized():
            _json_response(self, 401, {"error": "unauthorized"})
            return
        if self.path == "/v1/upload":
            try:
                request = self._read_json()
                encoded = request.get("data")
                if not isinstance(encoded, str):
                    raise BridgeError("data must be base64 text")
                payload = base64.b64decode(encoded, validate=True)
                result = self.server.store.put(request.get("name", "asset.usda"), payload,
                                               request.get("sha256"))
                _json_response(self, 201, result)
            except (BridgeError, ValueError) as exc:
                _json_response(self, 400, {"error": str(exc)})
            return
        _json_response(self, 404, {"error": "not found"})


class BridgeHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address: tuple[str, int], store: BridgeStore, token: str = "", verbose: bool = False):
        self.store = store
        self.token = token
        self.verbose = verbose
        self.max_request_bytes = store.max_bytes * 2 + 1024 * 1024
        super().__init__(address, _Handler)


class BridgeServer:
    """Threaded bridge lifecycle wrapper."""

    def __init__(self, root: str, host: str = "127.0.0.1", port: int = 8765,
                 token: str = "", max_bytes: int = 512 * 1024 * 1024, verbose: bool = False):
        self.httpd = BridgeHTTPServer((host, int(port)), BridgeStore(root, max_bytes), token, verbose)
        self.thread: threading.Thread | None = None

    @property
    def address(self) -> tuple[str, int]:
        return self.httpd.server_address

    def start(self) -> None:
        self.thread = threading.Thread(target=self.httpd.serve_forever, name="lightusd-asset-bridge", daemon=True)
        self.thread.start()

    def stop(self) -> None:
        self.httpd.shutdown()
        self.httpd.server_close()
        if self.thread:
            self.thread.join(timeout=2)

    def serve_forever(self) -> None:
        self.httpd.serve_forever()


def upload_http(url: str, name: str, payload: bytes, token: str = "") -> dict[str, Any]:
    parsed = urllib.parse.urlparse(url.rstrip("/") + "/v1/upload")
    body = json.dumps({"name": name, "data": base64.b64encode(payload).decode("ascii"),
                       "sha256": hashlib.sha256(payload).hexdigest()}).encode()
    conn = http.client.HTTPConnection(parsed.hostname, parsed.port or 80, timeout=30)
    headers = {"Content-Type": "application/json", "Content-Length": str(len(body))}
    if token:
        headers["X-LightUSD-Bridge-Token"] = token
    conn.request("POST", parsed.path, body, headers)
    response = conn.getresponse()
    value = json.loads(response.read())
    if response.status >= 300:
        raise BridgeError(value.get("error", f"HTTP {response.status}"))
    return value


def download_http(url: str, asset_id: str, token: str = "") -> tuple[dict[str, Any], bytes]:
    parsed = urllib.parse.urlparse(url.rstrip("/") + "/v1/download/" + asset_id)
    conn = http.client.HTTPConnection(parsed.hostname, parsed.port or 80, timeout=30)
    headers = {"Accept": "application/json"}
    if token:
        headers["X-LightUSD-Bridge-Token"] = token
    conn.request("GET", parsed.path, headers=headers)
    response = conn.getresponse()
    value = json.loads(response.read())
    if response.status >= 300:
        raise BridgeError(value.get("error", f"HTTP {response.status}"))
    payload = base64.b64decode(value.pop("data"), validate=True)
    if hashlib.sha256(payload).hexdigest() != value["sha256"]:
        raise BridgeError("download checksum mismatch")
    return value, payload


def _file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def upload_file_http(url: str, path: str | os.PathLike[str], token: str = "",
                     *, upload_name: str | None = None) -> dict[str, Any]:
    """Stream a file upload and return its content-addressed metadata."""
    source = Path(path)
    digest = _file_sha256(source)
    target = url.rstrip("/") + "/v1/upload-raw?" + urllib.parse.urlencode(
        {"name": upload_name or source.name})
    parsed = urllib.parse.urlparse(target)
    conn = http.client.HTTPConnection(parsed.hostname, parsed.port or 80, timeout=30)
    headers = {"Content-Type": "application/octet-stream",
               "Content-Length": str(source.stat().st_size),
               "X-LightUSD-SHA256": digest}
    if token:
        headers["X-LightUSD-Bridge-Token"] = token
    with source.open("rb") as stream:
        conn.request("PUT", parsed.path + "?" + parsed.query, stream, headers)
        response = conn.getresponse()
        value = json.loads(response.read())
    if response.status >= 300:
        raise BridgeError(value.get("error", f"HTTP {response.status}"))
    return value


def download_file_http(url: str, asset_id: str, path: str | os.PathLike[str], token: str = "") -> dict[str, Any]:
    """Stream, checksum-verify, and atomically store an asset."""
    parsed = urllib.parse.urlparse(
        url.rstrip("/") + "/v1/download-raw/" + asset_id)
    conn = http.client.HTTPConnection(parsed.hostname, parsed.port or 80, timeout=30)
    headers = {}
    if token:
        headers["X-LightUSD-Bridge-Token"] = token
    conn.request("GET", parsed.path, headers=headers)
    response = conn.getresponse()
    if response.status >= 300:
        value = json.loads(response.read())
        raise BridgeError(value.get("error", f"HTTP {response.status}"))
    destination = Path(path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".part")
    digest = hashlib.sha256()
    size = 0
    try:
        with temporary.open("wb") as stream:
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                stream.write(chunk)
                digest.update(chunk)
                size += len(chunk)
        expected = response.getheader("X-LightUSD-SHA256", "")
        if digest.hexdigest() != expected or expected != asset_id:
            raise BridgeError("download checksum mismatch")
        declared_size = int(response.getheader("Content-Length", "-1"))
        if declared_size != size:
            raise BridgeError("download size mismatch")
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)
    return {"id": asset_id,
            "name": response.getheader("X-LightUSD-Name", destination.name),
            "size": size, "sha256": expected}


def _bundle_path(path: str) -> str:
    """Validate and normalize a portable bundle-relative path."""
    value = str(path).replace("\\", "/")
    parts = [part for part in value.split("/") if part not in ("", ".")]
    if not parts or value.startswith("/") or any(part == ".." for part in parts):
        raise BridgeError("bundle paths must be safe relative paths")
    if ":" in parts[0]:
        raise BridgeError("bundle paths must not contain drive names")
    return "/".join(parts)


def build_bundle(files: dict[str, bytes]) -> bytes:
    """Build a deterministic ZIP bundle with a checksummed JSON manifest."""
    entries = []
    normalized: dict[str, bytes] = {}
    portable_names: set[str] = set()
    for relative, payload in files.items():
        name = _bundle_path(relative)
        portable_name = name.casefold()
        if (name == _BUNDLE_MANIFEST or name in normalized or
                portable_name in portable_names):
            raise BridgeError("duplicate or reserved bundle path")
        portable_names.add(portable_name)
        data = bytes(payload)
        normalized[name] = data
        entries.append({"path": name, "size": len(data),
                        "sha256": hashlib.sha256(data).hexdigest()})
    entries.sort(key=lambda item: item["path"])
    manifest = {"format": "lightusd-asset-bundle-v1", "files": entries}
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(_BUNDLE_MANIFEST,
                         json.dumps(manifest, sort_keys=True, separators=(",", ":")))
        for entry in entries:
            archive.writestr("files/" + entry["path"], normalized[entry["path"]])
    return output.getvalue()


def read_bundle(payload: bytes, max_bytes: int = 2 * 1024 * 1024 * 1024) -> tuple[dict[str, Any], dict[str, bytes]]:
    """Validate a bundle manifest and return its relative-path payloads."""
    try:
        with zipfile.ZipFile(io.BytesIO(payload), "r") as archive:
            manifest = json.loads(archive.read(_BUNDLE_MANIFEST))
            if manifest.get("format") != "lightusd-asset-bundle-v1":
                raise BridgeError("unsupported bundle format")
            files: dict[str, bytes] = {}
            portable_names: set[str] = set()
            total = 0
            for entry in manifest.get("files", []):
                name = _bundle_path(entry["path"])
                data = archive.read("files/" + name)
                total += len(data)
                if total > max_bytes:
                    raise BridgeError("expanded bundle exceeds size limit")
                if len(data) != int(entry["size"]):
                    raise BridgeError("bundle entry size mismatch: " + name)
                if hashlib.sha256(data).hexdigest() != entry["sha256"]:
                    raise BridgeError("bundle entry checksum mismatch: " + name)
                if name in files:
                    raise BridgeError("duplicate bundle entry: " + name)
                portable_name = name.casefold()
                if portable_name in portable_names:
                    raise BridgeError("portable bundle path collision: " + name)
                portable_names.add(portable_name)
                files[name] = data
    except (KeyError, json.JSONDecodeError, zipfile.BadZipFile) as exc:
        raise BridgeError("invalid LightUSD asset bundle") from exc
    return manifest, files


def upload_bundle_http(url: str, paths: list[str | os.PathLike[str]],
                       root: str | os.PathLike[str], token: str = "",
                       name: str = "lightusd-assets.lusdbundle") -> dict[str, Any]:
    """Upload files with paths relative to ``root`` as one verified bundle."""
    base = Path(root).resolve()
    sources: dict[str, Path] = {}
    portable_names: set[str] = set()
    for source_value in paths:
        source = Path(source_value).resolve()
        try:
            relative = source.relative_to(base).as_posix()
        except ValueError as exc:
            raise BridgeError("bundle source is outside root: " + str(source)) from exc
        portable_name = relative.casefold()
        if portable_name in portable_names:
            raise BridgeError("portable bundle path collision: " + relative)
        portable_names.add(portable_name)
        sources[relative] = source
    entries = [{"path": relative, "size": source.stat().st_size,
                "sha256": _file_sha256(source)}
               for relative, source in sorted(sources.items())]
    manifest = {"format": "lightusd-asset-bundle-v1", "files": entries}
    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
                prefix="lightusd-bundle-", suffix=".lusdbundle",
                delete=False) as temporary:
            temporary_path = Path(temporary.name)
        with zipfile.ZipFile(temporary_path, "w",
                             compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr(
                _BUNDLE_MANIFEST,
                json.dumps(manifest, sort_keys=True, separators=(",", ":")))
            for relative, source in sorted(sources.items()):
                archive.write(source, "files/" + relative)
        result = upload_file_http(
            url, temporary_path, token, upload_name=name)
        result["bundle_files"] = sorted(sources)
        return result
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)


def discover_asset_dependencies(root_layer: str | os.PathLike[str],
                                root: str | os.PathLike[str] | None = None,
                                *, strict: bool = True,
                                max_files: int = 10000,
                                max_total_bytes: int = 8 * 1024 * 1024 * 1024
                                ) -> list[Path]:
    """Return a deterministic transitive dependency closure for an asset.

    USDA/USD text asset paths and MaterialX file attributes are followed
    recursively. ``<UDIM>`` references expand to every four-digit tile. All
    resolved files must remain below ``root`` so a bundle cannot accidentally
    capture unrelated host files. Binary USDC dependency discovery requires a
    text sidecar/export and is rejected in strict mode.
    """
    entry = Path(root_layer).resolve()
    base = Path(root).resolve() if root is not None else entry.parent
    try:
        entry.relative_to(base)
    except ValueError as exc:
        raise BridgeError("root layer is outside dependency root: " + str(entry)) from exc
    if not entry.is_file():
        raise BridgeError("root layer does not exist: " + str(entry))

    found: set[Path] = set()
    portable_paths: dict[str, Path] = {}
    total_bytes = 0
    pending = [entry]
    while pending:
        source = pending.pop()
        if source in found:
            continue
        relative = source.relative_to(base).as_posix()
        portable_key = relative.casefold()
        previous = portable_paths.get(portable_key)
        if previous is not None and previous != source:
            raise BridgeError(
                f"dependency paths collide on Windows: {previous} and {source}")
        portable_paths[portable_key] = source
        found.add(source)
        if len(found) > int(max_files):
            raise BridgeError("dependency closure exceeds file-count limit")
        total_bytes += source.stat().st_size
        if total_bytes > int(max_total_bytes):
            raise BridgeError("dependency closure exceeds byte-size limit")
        suffix = source.suffix.lower()
        if suffix == ".usdz":
            references = _usdz_external_references(source)
        elif suffix in _TEXT_DEPENDENCY_SUFFIXES:
            references = _dependency_references(
                source.read_bytes(), suffix, str(source))
        else:
            continue
        for reference in references:
            # Package members, URLs and UE object paths are not filesystem
            # dependencies transported by this bridge.
            if not reference or "://" in reference or reference.startswith("/"):
                continue
            outer = reference.split("[", 1)[0]
            candidate_text = os.path.normpath(os.path.join(source.parent, outer))
            candidates = []
            if "<UDIM>" in candidate_text:
                pattern = glob.escape(candidate_text).replace(
                    glob.escape("<UDIM>"), "[0-9][0-9][0-9][0-9]")
                candidates = [Path(item).resolve() for item in glob.glob(pattern)]
            else:
                candidates = [Path(candidate_text).resolve()]
            if not candidates:
                if strict:
                    raise BridgeError("dependency has no matching UDIM tiles: " + reference)
                continue
            for candidate in candidates:
                try:
                    candidate.relative_to(base)
                except ValueError as exc:
                    raise BridgeError("dependency is outside root: " + str(candidate)) from exc
                if not candidate.is_file():
                    if strict:
                        raise BridgeError("dependency does not exist: " + str(candidate))
                    continue
                if candidate not in found:
                    pending.append(candidate)
    return sorted(found, key=lambda path: path.relative_to(base).as_posix())


def upload_asset_bundle_http(url: str, root_layer: str | os.PathLike[str],
                             root: str | os.PathLike[str] | None = None,
                             token: str = "",
                             name: str = "lightusd-assets.lusdbundle",
                             *, strict: bool = True,
                             additional_paths: list[str | os.PathLike[str]] | None = None,
                             max_files: int = 10000,
                             max_total_bytes: int = 8 * 1024 * 1024 * 1024
                             ) -> dict[str, Any]:
    """Discover and upload a root USD/MaterialX asset and its dependencies."""
    source = Path(root_layer).resolve()
    base = Path(root).resolve() if root is not None else source.parent
    paths = discover_asset_dependencies(
        source, base, strict=strict, max_files=max_files,
        max_total_bytes=max_total_bytes)
    paths.extend(Path(item).resolve() for item in (additional_paths or []))
    paths = sorted(set(paths))
    for path in paths:
        try:
            path.relative_to(base)
        except ValueError as exc:
            raise BridgeError("bundle source is outside root: " + str(path)) from exc
    if len(paths) > int(max_files):
        raise BridgeError("asset bundle exceeds file-count limit")
    if sum(path.stat().st_size for path in paths) > int(max_total_bytes):
        raise BridgeError("asset bundle exceeds byte-size limit")
    result = upload_bundle_http(url, paths, base, token, name)
    result["root_layer"] = source.relative_to(base).as_posix()
    return result


def download_bundle_http(url: str, asset_id: str, destination: str | os.PathLike[str],
                         token: str = "", max_bytes: int = 2 * 1024 * 1024 * 1024) -> dict[str, Any]:
    """Download, verify, and safely extract a LightUSD asset bundle."""
    root = Path(destination)
    root.mkdir(parents=True, exist_ok=True)
    bundle_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
                dir=root, prefix="bundle-", suffix=".part",
                delete=False) as temporary:
            bundle_path = Path(temporary.name)
        metadata = download_file_http(url, asset_id, bundle_path, token)
        metadata["bundle"] = extract_bundle_file(
            bundle_path, root, max_bytes)
        return metadata
    finally:
        if bundle_path is not None:
            bundle_path.unlink(missing_ok=True)


def extract_bundle_file(path: str | os.PathLike[str],
                        destination: str | os.PathLike[str],
                        max_bytes: int = 2 * 1024 * 1024 * 1024
                        ) -> dict[str, Any]:
    """Validate and stream-extract a bundle without buffering its entries."""
    root = Path(destination)
    root.mkdir(parents=True, exist_ok=True)
    try:
        with zipfile.ZipFile(path, "r") as archive:
            manifest_info = archive.getinfo(_BUNDLE_MANIFEST)
            if manifest_info.file_size > min(max_bytes, 16 * 1024 * 1024):
                raise BridgeError("bundle manifest exceeds size limit")
            manifest = json.loads(archive.read(manifest_info))
            if manifest.get("format") != "lightusd-asset-bundle-v1":
                raise BridgeError("unsupported bundle format")
            validated: list[tuple[dict[str, Any], str]] = []
            portable_names: set[str] = set()
            declared_total = 0
            for entry in manifest.get("files", []):
                name = _bundle_path(entry["path"])
                portable_name = name.casefold()
                if portable_name in portable_names:
                    raise BridgeError("portable bundle path collision: " + name)
                portable_names.add(portable_name)
                declared_size = int(entry["size"])
                if declared_size < 0:
                    raise BridgeError("bundle entry has negative size: " + name)
                declared_total += declared_size
                if declared_total > max_bytes:
                    raise BridgeError("expanded bundle exceeds size limit")
                info = archive.getinfo("files/" + name)
                if info.file_size != declared_size:
                    raise BridgeError("bundle entry size mismatch: " + name)
                validated.append((entry, name))

            extracted_total = 0
            with tempfile.TemporaryDirectory(
                    dir=root, prefix="extract-") as staging_value:
                staging = Path(staging_value)
                for entry, name in validated:
                    temporary = staging.joinpath(*name.split("/"))
                    temporary.parent.mkdir(parents=True, exist_ok=True)
                    digest = hashlib.sha256()
                    size = 0
                    with archive.open("files/" + name, "r") as source, \
                            temporary.open("wb") as output:
                        while True:
                            chunk = source.read(1024 * 1024)
                            if not chunk:
                                break
                            size += len(chunk)
                            extracted_total += len(chunk)
                            if extracted_total > max_bytes:
                                raise BridgeError(
                                    "expanded bundle exceeds size limit")
                            output.write(chunk)
                            digest.update(chunk)
                    if size != int(entry["size"]):
                        raise BridgeError("bundle entry size mismatch: " + name)
                    if digest.hexdigest() != entry["sha256"]:
                        raise BridgeError(
                            "bundle entry checksum mismatch: " + name)
                for _, name in validated:
                    source = staging.joinpath(*name.split("/"))
                    target = root.joinpath(*name.split("/"))
                    target.parent.mkdir(parents=True, exist_ok=True)
                    source.replace(target)
    except (KeyError, TypeError, ValueError, json.JSONDecodeError,
            zipfile.BadZipFile) as exc:
        raise BridgeError("invalid LightUSD asset bundle") from exc
    return manifest


def _ws_frame(payload: bytes, opcode: int = 1) -> bytes:
    length = len(payload)
    if length < 126:
        header = struct.pack("!BB", 0x80 | opcode, length)
    elif length < 65536:
        header = struct.pack("!BBH", 0x80 | opcode, 126, length)
    else:
        header = struct.pack("!BBQ", 0x80 | opcode, 127, length)
    return header + payload


def _ws_read(sock: socket.socket) -> tuple[int, bytes]:
    head = sock.recv(2)
    if len(head) != 2:
        raise ConnectionError("incomplete websocket frame")
    first, second = head
    length = second & 0x7F
    if length == 126:
        length = struct.unpack("!H", sock.recv(2))[0]
    elif length == 127:
        length = struct.unpack("!Q", sock.recv(8))[0]
    mask = sock.recv(4) if second & 0x80 else b""
    data = bytearray()
    while len(data) < length:
        chunk = sock.recv(min(65536, length - len(data)))
        if not chunk:
            raise ConnectionError("incomplete websocket payload")
        data.extend(chunk)
    if mask:
        for i in range(length):
            data[i] ^= mask[i % 4]
    return first & 0x0F, bytes(data)


def _websocket_session(handler: _Handler, server: BridgeHTTPServer) -> None:
    key = handler.headers.get("Sec-WebSocket-Key", "")
    if not key:
        _json_response(handler, 400, {"error": "missing websocket key"})
        return
    accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
    handler.send_response(101, "Switching Protocols")
    handler.send_header("Upgrade", "websocket")
    handler.send_header("Connection", "Upgrade")
    handler.send_header("Sec-WebSocket-Accept", accept)
    handler.end_headers()
    sock = handler.connection
    while True:
        try:
            opcode, payload = _ws_read(sock)
        except (ConnectionError, OSError):
            break
        if opcode == 8:
            break
        if opcode == 9:
            sock.sendall(_ws_frame(payload, 10))
            continue
        if opcode != 1:
            continue
        try:
            request = json.loads(payload)
            if request.get("op") == "health":
                response = {"ok": True, "protocol": "lightusd-asset-bridge-v1"}
            elif request.get("op") == "upload":
                data = base64.b64decode(request["data"], validate=True)
                response = server.store.put(request.get("name", "asset.usda"), data, request.get("sha256"))
            elif request.get("op") == "download":
                response, data = server.store.get(request["id"])
                response["data"] = base64.b64encode(data).decode("ascii")
            else:
                raise BridgeError("unknown websocket operation")
        except (BridgeError, KeyError, ValueError, json.JSONDecodeError) as exc:
            response = {"error": str(exc)}
        sock.sendall(_ws_frame(json.dumps(response, separators=(",", ":")).encode()))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="LightUSD HTTP/WebSocket asset bridge")
    parser.add_argument("--root", default=".lightusd-bridge")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--token", default="", help="shared token; omit only on a trusted local interface")
    parser.add_argument("--max-mb", type=int, default=512)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)
    server = BridgeServer(args.root, args.host, args.port, args.token, args.max_mb * 1024 * 1024, args.verbose)
    print(f"LightUSD asset bridge listening on http://{args.host}:{args.port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        return 0
    finally:
        server.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

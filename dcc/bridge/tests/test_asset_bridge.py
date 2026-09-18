"""Smoke tests for the dependency-free DCC asset bridge."""

import base64
import hashlib
import json
import os
import socket
import tempfile
import unittest

from dcc.bridge.asset_bridge import (BridgeError, BridgeServer, build_bundle,
                                     download_file_http,
                                     download_bundle_http, download_http,
                                     upload_bundle_http, upload_file_http,
                                     upload_http)


def client_frame(payload: bytes) -> bytes:
    mask = b"test"
    length = len(payload)
    if length < 126:
        head = bytes([0x81, 0x80 | length])
    else:
        head = bytes([0x81, 0xFE]) + length.to_bytes(2, "big")
    encoded = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
    return head + mask + encoded


def read_frame(sock: socket.socket) -> bytes:
    first, second = sock.recv(2)
    length = second & 0x7F
    if length == 126:
        length = int.from_bytes(sock.recv(2), "big")
    data = b""
    while len(data) < length:
        data += sock.recv(length - len(data))
    return data


class AssetBridgeTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.server = BridgeServer(self.temp.name, host="127.0.0.1", port=0,
                                   token="secret", max_bytes=4096)
        self.server.start()
        self.url = f"http://127.0.0.1:{self.server.address[1]}"

    def tearDown(self):
        self.server.stop()
        self.temp.cleanup()

    def test_http_roundtrip(self):
        payload = b"#usda 1.0\ndef Xform \"Root\" {}\n"
        uploaded = upload_http(self.url, "scene.usda", payload, "secret")
        self.assertEqual(uploaded["size"], len(payload))
        meta, received = download_http(self.url, uploaded["id"], "secret")
        self.assertEqual(meta["name"], "scene.usda")
        self.assertEqual(received, payload)

    def test_websocket_roundtrip(self):
        host, port = self.server.address
        sock = socket.create_connection((host, port))
        key = base64.b64encode(b"0123456789012345").decode()
        request = (
            f"GET /v1/ws HTTP/1.1\r\nHost: {host}:{port}\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
            "X-LightUSD-Bridge-Token: secret\r\n\r\n")
        sock.sendall(request.encode())
        response = b""
        while b"\r\n\r\n" not in response:
            response += sock.recv(1024)
        payload = b"abc.usda"
        request = {"op": "upload", "name": "abc.usda",
                   "data": base64.b64encode(payload).decode(),
                   "sha256": hashlib.sha256(payload).hexdigest()}
        sock.sendall(client_frame(json.dumps(request).encode()))
        reply = json.loads(read_frame(sock))
        self.assertEqual(reply["size"], len(payload))
        sock.close()

    def test_file_helpers(self):
        source = os.path.join(self.temp.name, "source.usda")
        destination = os.path.join(self.temp.name, "nested", "copy.usda")
        payload = b"#usda 1.0\ndef Xform \"Bridge\" {}\n"
        with open(source, "wb") as stream:
            stream.write(payload)
        uploaded = upload_file_http(self.url, source, "secret")
        downloaded = download_file_http(self.url, uploaded["id"], destination, "secret")
        self.assertEqual(downloaded["sha256"], uploaded["sha256"])
        with open(destination, "rb") as stream:
            self.assertEqual(stream.read(), payload)

    def test_dependency_bundle_roundtrip(self):
        source_root = os.path.join(self.temp.name, "source")
        destination = os.path.join(self.temp.name, "extracted")
        os.makedirs(os.path.join(source_root, "textures"))
        assets = {
            "scene.usda": b'#usda 1.0\ndef Xform "Bundle" {}\n',
            "materials/look.mtlx": b"<materialx version=\"1.39\"/>",
            "textures/skin.1001.png": b"tile-1001",
            "textures/skin.1002.png": b"tile-1002",
        }
        paths = []
        for relative, payload in assets.items():
            path = os.path.join(source_root, *relative.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as stream:
                stream.write(payload)
            paths.append(path)
        uploaded = upload_bundle_http(self.url, paths, source_root, "secret")
        metadata = download_bundle_http(self.url, uploaded["id"], destination, "secret")
        self.assertEqual(metadata["bundle"]["format"], "lightusd-asset-bundle-v1")
        self.assertEqual(sorted(assets), [entry["path"] for entry in metadata["bundle"]["files"]])
        for relative, payload in assets.items():
            with open(os.path.join(destination, *relative.split("/")), "rb") as stream:
                self.assertEqual(stream.read(), payload)

    def test_dependency_bundle_rejects_unsafe_paths(self):
        for path in ("../escape.usda", "/absolute.usda", "C:/drive.usda"):
            with self.subTest(path=path), self.assertRaises(BridgeError):
                build_bundle({path: b"unsafe"})


if __name__ == "__main__":
    unittest.main()

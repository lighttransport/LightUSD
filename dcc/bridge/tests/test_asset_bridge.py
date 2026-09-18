"""Smoke tests for the dependency-free DCC asset bridge."""

import base64
import hashlib
import http.client
import json
import os
import socket
import tempfile
import unittest
import zipfile

from dcc.bridge.asset_bridge import (BridgeError, BridgeServer, build_bundle,
                                     discover_asset_dependencies,
                                     download_file_http,
                                     download_bundle_http, download_http,
                                     upload_asset_bundle_http,
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
                                   token="secret", max_bytes=4 * 1024 * 1024)
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

    def test_streams_large_file_and_rejects_bad_checksum(self):
        source = os.path.join(self.temp.name, "large.bin")
        destination = os.path.join(self.temp.name, "large-copy.bin")
        payload = (bytes(range(256)) * 8193) + b"tail"
        with open(source, "wb") as stream:
            stream.write(payload)
        uploaded = upload_file_http(self.url, source, "secret")
        download_file_http(self.url, uploaded["id"], destination, "secret")
        with open(destination, "rb") as stream:
            self.assertEqual(hashlib.sha256(stream.read()).hexdigest(),
                             uploaded["sha256"])

        host, port = self.server.address
        connection = http.client.HTTPConnection(host, port, timeout=30)
        connection.request(
            "PUT", "/v1/upload-raw?name=bad.bin", b"bad",
            {"Content-Length": "3", "X-LightUSD-SHA256": "0" * 64,
             "X-LightUSD-Bridge-Token": "secret"})
        response = connection.getresponse()
        self.assertEqual(response.status, 400)
        response.read()
        self.assertFalse(any(
            name.endswith(".part")
            for _, _, names in os.walk(self.temp.name) for name in names))

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

        rejected = os.path.join(self.temp.name, "rejected")
        with self.assertRaises(BridgeError):
            download_bundle_http(
                self.url, uploaded["id"], rejected, "secret", max_bytes=5)
        self.assertEqual([], os.listdir(rejected))

    def test_dependency_bundle_rejects_unsafe_paths(self):
        for path in ("../escape.usda", "/absolute.usda", "C:/drive.usda"):
            with self.subTest(path=path), self.assertRaises(BridgeError):
                build_bundle({path: b"unsafe"})
        with self.assertRaises(BridgeError):
            build_bundle({"Textures/Skin.png": b"a",
                          "textures/skin.png": b"b"})

    def test_discovers_recursive_materialx_and_udim_dependencies(self):
        root = os.path.join(self.temp.name, "closure")
        os.makedirs(os.path.join(root, "looks"))
        os.makedirs(os.path.join(root, "textures"))
        files = {
            "scene.usda": b'#usda 1.0\n( subLayers = [@layout.usda@] )\nasset mtlx = @looks/skin.mtlx@\n',
            "layout.usda": b'#usda 1.0\ndef Xform "Root" {}\n',
            "looks/skin.mtlx": b'<materialx><image file="../textures/skin.&lt;UDIM&gt;.png"/></materialx>',
            "textures/skin.1001.png": b"one",
            "textures/skin.1002.png": b"two",
        }
        for relative, payload in files.items():
            path = os.path.join(root, relative)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as stream:
                stream.write(payload)
        paths = discover_asset_dependencies(os.path.join(root, "scene.usda"), root)
        self.assertEqual(sorted(files), [path.relative_to(root).as_posix() for path in paths])
        uploaded = upload_asset_bundle_http(
            self.url, os.path.join(root, "scene.usda"), root, "secret")
        self.assertEqual("scene.usda", uploaded["root_layer"])
        self.assertEqual(sorted(files), uploaded["bundle_files"])

    def test_dependency_discovery_rejects_escape_and_missing(self):
        root = os.path.join(self.temp.name, "safe")
        os.makedirs(root)
        outside = os.path.join(self.temp.name, "outside.usda")
        with open(outside, "wb") as stream:
            stream.write(b"#usda 1.0\n")
        scene = os.path.join(root, "scene.usda")
        with open(scene, "wb") as stream:
            stream.write(b'#usda 1.0\nasset bad = @../outside.usda@\n')
        with self.assertRaises(BridgeError):
            discover_asset_dependencies(scene, root)
        with open(scene, "wb") as stream:
            stream.write(b'#usda 1.0\nasset missing = @missing.png@\n')
        with self.assertRaises(BridgeError):
            discover_asset_dependencies(scene, root)

    def test_dependency_cycles_limits_and_case_collisions(self):
        root = os.path.join(self.temp.name, "limits")
        os.makedirs(os.path.join(root, "Textures"))
        os.makedirs(os.path.join(root, "textures"))
        a = os.path.join(root, "a.usda")
        b = os.path.join(root, "b.usda")
        with open(a, "wb") as stream:
            stream.write(b'#usda 1.0\n( subLayers = [@b.usda@] )\n')
        with open(b, "wb") as stream:
            stream.write(b'#usda 1.0\n( subLayers = [@a.usda@] )\n')
        self.assertEqual(["a.usda", "b.usda"], [
            path.relative_to(root).as_posix()
            for path in discover_asset_dependencies(a, root)])
        with self.assertRaises(BridgeError):
            discover_asset_dependencies(a, root, max_files=1)
        with self.assertRaises(BridgeError):
            discover_asset_dependencies(a, root, max_total_bytes=8)

        upper = os.path.join(root, "Textures", "Skin.png")
        lower = os.path.join(root, "textures", "skin.png")
        for path in (upper, lower):
            with open(path, "wb") as stream:
                stream.write(b"x")
        with open(a, "wb") as stream:
            stream.write(
                b'#usda 1.0\nasset a = @Textures/Skin.png@\n'
                b'asset b = @textures/skin.png@\n')
        with self.assertRaises(BridgeError):
            discover_asset_dependencies(a, root)

    def test_discovers_binary_usdc_dependencies(self):
        import lightusd
        root = os.path.join(self.temp.name, "binary")
        os.makedirs(root)
        dependency = os.path.join(root, "payload.usda")
        with open(dependency, "wb") as stream:
            stream.write(b'#usda 1.0\ndef Xform "Payload" {}\n')
        stage = lightusd.Stage.create()
        stage.add_sublayer("payload.usda")
        usdc = os.path.join(root, "scene.usdc")
        with open(usdc, "wb") as stream:
            stream.write(stage.export_usdc())
        paths = discover_asset_dependencies(usdc, root)
        self.assertEqual(["payload.usda", "scene.usdc"],
                         [path.relative_to(root).as_posix() for path in paths])

    def test_discovers_usdz_external_and_materialx_include(self):
        root = os.path.join(self.temp.name, "package")
        os.makedirs(os.path.join(root, "shared"))
        external = os.path.join(root, "external.usda")
        include = os.path.join(root, "shared", "library.mtlx")
        with open(external, "wb") as stream:
            stream.write(b'#usda 1.0\ndef Xform "External" {}\n')
        with open(include, "wb") as stream:
            stream.write(b'<materialx version="1.39"/>')
        package = os.path.join(root, "scene.usdz")
        with zipfile.ZipFile(package, "w") as archive:
            archive.writestr("root.usda",
                             '#usda 1.0\nasset local = @textures/a.png@\n'
                             'asset external = @external.usda@\n'
                             'asset remote = @https://example.invalid/a.usd@\n')
            archive.writestr("textures/a.png", b"image")
            archive.writestr("looks/look.mtlx",
                             '<materialx><xi:include href="../shared/library.mtlx"/>'
                             '</materialx>')
        paths = discover_asset_dependencies(package, root)
        self.assertEqual(["external.usda", "scene.usdz", "shared/library.mtlx"],
                         [path.relative_to(root).as_posix() for path in paths])


if __name__ == "__main__":
    unittest.main()

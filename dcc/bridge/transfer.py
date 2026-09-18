"""Tiny DCC-facing wrappers for the LightUSD asset bridge."""

from __future__ import annotations

import os
import sys
from pathlib import Path


def _bridge():
    # UE's embedded Python and Blender's add-on Python may not have the repo
    # root on sys.path when called from an MCP tool.
    root = os.environ.get("LIGHTUSD_REPO_ROOT")
    if root and root not in sys.path:
        sys.path.insert(0, root)
    from .asset_bridge import (download_bundle_http, download_file_http,
                               upload_bundle_http, upload_file_http)
    return download_file_http, upload_file_http, download_bundle_http, upload_bundle_http


def upload_file(url: str, path: str | os.PathLike[str], token: str = ""):
    """Upload a USD/texture/cache file through the HTTP fallback."""
    _, upload, _, _ = _bridge()
    return upload(url, Path(path), token)


def download_file(url: str, asset_id: str, path: str | os.PathLike[str], token: str = ""):
    """Download and checksum-verify a USD/texture/cache file."""
    download, _, _, _ = _bridge()
    return download(url, asset_id, Path(path), token)


def upload_bundle(url: str, paths, root: str | os.PathLike[str], token: str = ""):
    """Upload USD and its dependencies as a checksummed bundle."""
    _, _, _, upload = _bridge()
    return upload(url, list(paths), root, token)


def download_bundle(url: str, asset_id: str, destination: str | os.PathLike[str], token: str = ""):
    """Download and safely extract a checksummed dependency bundle."""
    _, _, download, _ = _bridge()
    return download(url, asset_id, destination, token)

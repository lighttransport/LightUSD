"""Small cross-DCC asset transfer bridge used by the UE/Blender workflow."""

__all__ = ["BridgeServer", "BridgeStore", "download_http", "upload_http",
           "download_file_http", "upload_file_http"]


def __getattr__(name):
    # Keep ``python -m dcc.bridge.asset_bridge`` free of the runpy warning
    # caused by eagerly importing the CLI module from the package initializer.
    if name in __all__:
        from . import asset_bridge
        return getattr(asset_bridge, name)
    raise AttributeError(name)

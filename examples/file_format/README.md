# Custom format adapter on core

Build with `LIGHTUSD_BUILD_EXAMPLES=ON` and run
`build_ninja/file_format_example`. The default example is self-contained;
an optional argument supplies a USD root layer containing references to
`bora.my` or `dora.my`.

The application owns a decoder for the `.my` format (one little-endian
binary32 value). It decodes each asset into a core `Layer`, registers the
asset identifier with `AssetResolver`, and calls `pcp::Cache::PreloadLayer`
before composing references. This is an embedding adapter, rather than a
global format-plugin registration. Decoding validates the byte count and
rejects non-finite values. The default scene verifies that both referenced
values composed successfully, then prints the composed USDA.

`core_file_format_example` runs the in-memory composition path in CTest.

# LightUSD

LightUSD is a full-featured, dependency-free, lightweight USD library written
in C++17. It reads and writes USDA, USDC, and USDZ, and includes MaterialX and USD Physics support.

LightUSD is designed for applications that need USD to be compact, portable,
and fast: high-performance 3D interchange and DCC tools, render and asset
pipelines, generative AI, and Physical AI systems that connect scene
understanding with simulation and robotics. The same core runs across desktop,
mobile, WebAssembly, and sandboxed environments, with explicit resource limits
for untrusted assets. MCP interface provided.

[![GitHub Sponsors](https://img.shields.io/badge/Sponsor-%E2%9D%A4-pink?logo=github)](https://github.com/sponsors/lighttransport)
[![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/lighttransport/lightusd)
[![PyPI version](https://img.shields.io/pypi/v/lightusd.svg)](https://pypi.org/project/lightusd/)
[![npm version](https://img.shields.io/npm/v/lightusd.svg)](https://www.npmjs.com/package/lightusd)

## Releases

* v1.0.0 RC (main)
  * Will be 26.09 or 26.10

## Packages

Python:

```sh
python -m pip install lightusd
```

```python
import lightusd

stage = lightusd.load("scene.usdz")
for prim in lightusd.traverse(stage):
    print(prim.type_name, prim.name)
```

See [python/README.md](python/README.md) for Python usage. WebAssembly and
JavaScript packages live under [web/](web/).

## Build

LightUSD requires a C++17 compiler and normally uses CMake:

```sh
cmake -S . -B build_ninja -G Ninja \
  -DLIGHTUSD_BUILD_TESTS=ON -DLIGHTUSD_BUILD_EXAMPLES=ON
cmake --build build_ninja
ctest --test-dir build_ninja --output-on-failure
```

See [doc/build-and-examples.md](doc/build-and-examples.md) for integration,
build options, and examples.

## Documentation

- [Documentation index](doc/README.md)
- [API and schema status](doc/api-status.md)
- [MaterialX](doc/materialx.md)
- [USD Physics](doc/usd-physics.md)
- [Composition](doc/composition.md)
- [Python bindings](doc/python_binding.md)
- [Testing](doc/testing-cpp.md)
- [Release and CI](doc/ci.md)

## Security

Use `USDLoadOptions::max_memory_limit_in_mb` for unknown inputs. For hostile or
internet-sourced assets, also isolate loading in a sandboxed process or WASI
environment.

## License

LightUSD is primarily licensed under Apache 2.0. Some helper code uses MIT or
similarly permissive licenses. See [LICENSE.3rdparty](LICENSE.3rdparty) for
third-party notices.

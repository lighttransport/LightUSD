# LightUSD Python Binding

LightUSD ships a CPython extension package named `lightusd` **v1.0.0**
(next core, `preview` npm dist-tag), built on the **next core**
(`src/next/` + `src/tydra/next/`) through the C API (`src/c-api/`). The
legacy lightusd core is not linked into the Python wheel. Native C++ (`cmake
-S .`) and WASM (`web/CMakeLists.txt` / `web/binding.cc` with
`LIGHTUSD_WASM_PRODUCT=legacy/next/combined`) still build and ship the legacy
path.

End-user package documentation lives in [../python/README.md](../python/README.md).
This page is for source builds and maintainer notes.

## Package Status

- Package name: `lightusd`
- Python support: CPython 3.10+ (stable ABI) and free-threaded CPython 3.14
- Wheels: `cp310-abi3` (one wheel covers 3.10+) and `cp314t` (free-threaded,
  `Py_mod_gil = Py_MOD_GIL_NOT_USED`)
- Runtime dependency on NumPy: none (zero-copy interop when present, via
  `__array_interface__` on the abi3 build and the buffer protocol on cp314t)
- Version source: git tags through `setuptools_scm`

## Architecture

```
python/lightusd/__init__.py   pure-python facade (pathlib, value normalizer)
python/lightusd/tydra.py      render-scene shim
src/python/py-*.c             raw CPython C-API extension (lightusd._core)
src/c-api/lightusd-c.*        core C API (lightusd_*): stage/prim/attr/authoring
src/c-api/lightusd-render-c.* tydra render C API (buffers, materials, nodes)
src/next/                     next core (parser, crate, composition, writers)
src/tydra/next/               render-scene converter
```

Single extension source, two build configurations:

- **abi3** (default): `Py_LIMITED_API=0x030A0000`. The buffer protocol is not
  in the limited API before 3.11, so zero-copy numpy interop goes through
  `__array_interface__` / `Array.memoryview()`.
- **free-threaded** (`Py_GIL_DISABLED` interpreters): non-limited build with
  real buffer-protocol slots. All extension state lives in module state
  (multi-phase init, no static globals); stage reads are thread-safe (lazy
  crate-array materialization is serialized inside the C API), authoring must
  not race reads of the same stage.

## Building from source

```bash
pip install -e .          # drives CMake on src/next, then builds the extension
pytest python/tests -q
```

### Recommended `uv` workflow

`uv` is the preferred way to select the repository Python and keep build/test
dependencies isolated. The interpreter selector may resolve to a free-threaded
CPython build when one is installed; this exercises the same configuration as
the free-threaded wheel job.

```bash
UV_CACHE_DIR=/tmp/uv-cache uv venv --python 3.14t .venv
UV_CACHE_DIR=/tmp/uv-cache uv pip install \
  --python .venv/bin/python setuptools setuptools_scm wheel pytest 'numpy>=2.2,<3'
UV_CACHE_DIR=/tmp/uv-cache uv pip install \
  --python .venv/bin/python -e '.[test]' --no-build-isolation
UV_CACHE_DIR=/tmp/uv-cache .venv/bin/python -m pytest python/tests -q
```

The explicit build-tool install is needed because the editable install uses
`--no-build-isolation`; it makes the procedure work in an offline or
pre-provisioned build environment without relying on `pip` to create a second
environment.

If a 3.14 free-threaded interpreter is unavailable, replace `3.14t` with the
CPython version selected by `uv python find` (3.10 or newer). Use the same
`.venv/bin/python` path for CTest:

```bash
cmake -S . -B build_ninja -G Ninja \
  -DLIGHTUSD_BUILD_TESTS=ON \
  -DLIGHTUSD_PYTHON_EXECUTABLE="$PWD/.venv/bin/python" \
  -DLIGHTUSD_BUILD_PYTHON_TESTS=ON
ctest --test-dir build_ninja -R '^python-lightusd-tests$' --output-on-failure
```

Environment overrides: `LIGHTUSD_PY_LIMITED_API=0` (force a non-abi3 dev
build), `LIGHTUSD_CMAKE_ARGS` (extra CMake args),
`LIGHTUSD_TEST_ASSETS` (pytest asset dir).

Wheels are built by `.github/workflows/wheels.yml` with cibuildwheel (v3.x,
`enable = ["cpython-freethreading"]`); configuration lives in
`[tool.cibuildwheel]` in `pyproject.toml`. Publishing uses PyPI Trusted
Publishing (OIDC), unchanged.

## C API notes

`src/c-api/lightusd-c.h` is a standalone C11 FFI surface usable from any
language (Rust/C#/Deno/...): opaque owning handles + by-value `lightusd_prim`
handles, thread-local `lightusd_last_error()`, zero-copy `lightusd_value_view` /
`lightusd_buffer_view` views, batched authoring calls. Smoke-tested from pure C
by `tests/c-api/test_lightusd_c.c` (ctest: `next_test_c_api`).

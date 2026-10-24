"""
Build driver for the lightusd PyPI wheel.

Flow:
  1. Invoke CMake on the standalone `src/next` project (next-core), building
     the next core, Tydra, asset importer, C API and native validation libraries.
     The legacy lightusd core is NOT built or linked into the Python wheel
     (v1.0.0 next + tydra-next only, npm preview tag). Native
     (`cmake -S .`) and WASM (`web/CMakeLists.txt` / `web/binding.cc` with
     LIGHTUSD_WASM_PRODUCT=legacy/next/combined) still build the legacy path.
  2. Compile the `src/python/py-*.c` sources into the `lightusd._core`
     extension, linking those static archives.

Two build flavors from the same sources:
  - Regular CPython: stable-ABI (abi3) extension with
    Py_LIMITED_API=0x030B0000; the wheel is tagged cp311-abi3 and covers
    CPython >= 3.11.
  - Free-threaded CPython (Py_GIL_DISABLED, e.g. 3.14t): the limited API is
    unavailable there, so a regular version-specific extension is built
    (wheel tag cp314t-...). The module declares Py_mod_gil=Py_MOD_GIL_NOT_USED.
"""
from __future__ import annotations

import os
import pathlib
import platform
import shlex
import shutil
import subprocess
import sysconfig
from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext


ROOT = pathlib.Path(__file__).parent.resolve()


def _env_flag(name: str, default: bool) -> bool:
    value = os.environ.get(name)
    if value is None:
        return default
    normalized = value.strip().lower()
    if normalized in {"1", "on", "true", "yes"}:
        return True
    if normalized in {"0", "off", "false", "no"}:
        return False
    raise RuntimeError(
        f"{name} must be one of 1/0, on/off, true/false, or yes/no")


# Worker-thread parallelism is enabled for published wheels. It can be disabled
# for constrained native targets without changing the C/Python API; internal
# synchronization required by free-threaded CPython remains available.
PY_ENABLE_THREAD = _env_flag("LIGHTUSD_PY_ENABLE_THREAD", True)
_default_build_dir = ("build_py_ext_next" if PY_ENABLE_THREAD else
                      "build_py_ext_next_nothread")
CMAKE_BUILD_DIR = pathlib.Path(
    os.environ.get("LIGHTUSD_PY_BUILD_DIR", str(ROOT / _default_build_dir))
).resolve()

PY_LIMITED_API = 0x030B0000  # CPython 3.11 floor for the abi3 wheel

# The stable ABI cannot be used on free-threaded CPython builds.
FREE_THREADED = bool(sysconfig.get_config_var("Py_GIL_DISABLED"))
USE_LIMITED = (not FREE_THREADED and
               os.environ.get("LIGHTUSD_PY_LIMITED_API", "1") != "0")


def _cmake_configure_and_build() -> None:
    CMAKE_BUILD_DIR.mkdir(parents=True, exist_ok=True)

    cmake_args = [
        "cmake",
        "-S", str(ROOT / "src" / "next"),
        "-B", str(CMAKE_BUILD_DIR),
        "-DCMAKE_BUILD_TYPE=MinSizeRel",
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
        ("-DLIGHTUSD_NEXT_ENABLE_THREAD=" +
         ("ON" if PY_ENABLE_THREAD else "OFF")),
        "-DLIGHTUSD_NEXT_BUILD_TESTS=OFF",
        "-DLIGHTUSD_NEXT_BUILD_EXAMPLES=OFF",
        "-DLIGHTUSD_WITH_TYDRA=ON",
    ]
    is_windows = platform.system() == "Windows"

    # On Windows, prefer the native Visual Studio generator so CMake
    # auto-locates MSVC without needing cl.exe on PATH. Elsewhere Ninja is
    # strictly faster if available.
    if not is_windows and shutil.which("ninja") is not None:
        cmake_args.extend(["-G", "Ninja"])
    if is_windows:
        cmake_args.append("-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL")

    extra = os.environ.get("LIGHTUSD_CMAKE_ARGS", "")
    if extra:
        cmake_args.extend(
            shlex.split(extra, posix=platform.system() != "Windows"))

    env = os.environ.copy()
    subprocess.check_call(cmake_args, env=env)

    build_args = [
        "cmake", "--build", str(CMAKE_BUILD_DIR),
        "--config", "MinSizeRel",
        "--target", "lightusd_next",
        "--target", "tydra_next",
        "--target", "lightusd_render_c",
        "--target", "lightusd_validation_c",
        "--parallel", os.environ.get("LIGHTUSD_PY_JOBS", str(min(16, os.cpu_count() or 2))),
    ]
    subprocess.check_call(build_args, env=env)


class CMakeBuildExt(build_ext):
    """Drive CMake to produce static libs, then compile the C extension."""

    def run(self) -> None:
        _cmake_configure_and_build()

        # CMake emits exact artifact paths in static link order. A recursive
        # filename search can pick stale archives left by a reconfiguration.
        manifest = CMAKE_BUILD_DIR / "lightusd-python-libraries-MinSizeRel.txt"
        archives = manifest.read_text().splitlines()
        if len(archives) != 10 or not all(pathlib.Path(p).is_file() for p in archives):
            raise RuntimeError(f"Invalid CMake archive manifest: {manifest}")
        stamp = pathlib.Path(self.build_temp) / "lightusd-build-config.txt"
        stamp.parent.mkdir(parents=True, exist_ok=True)
        config = repr((archives, USE_LIMITED, PY_LIMITED_API, PY_ENABLE_THREAD))
        if not stamp.exists() or stamp.read_text() != config:
            stamp.write_text(config)
        headers = [str(p) for directory in (ROOT / "src/python", ROOT / "src/c-api", ROOT / "src/next/c")
                   for p in directory.glob("*.h")]
        for ext in self.extensions:
            ext.extra_objects = archives + list(ext.extra_objects)
            ext.depends = list(ext.depends or []) + archives + headers + [str(stamp)]
        super().run()


extra_compile_args = []
extra_link_args = []
if platform.system() == "Linux":
    extra_compile_args += ["-O2", "-ffunction-sections", "-fdata-sections"]
    extra_link_args += ["-lstdc++", "-lm", "-lpthread",
                        "-Wl,--gc-sections", "-Wl,-s"]
elif platform.system() == "Darwin":
    extra_compile_args += ["-O2"]
    extra_link_args += ["-lc++", "-Wl,-dead_strip", "-Wl,-x"]
elif platform.system() == "Windows":
    # Explicit C standard: the .c sources use C99 compound literals and
    # designated initializers. Linux/Darwin get an implicit C99-superset
    # default from GCC/Clang, but MSVC needs this spelled out. This repo's
    # CI/build already targets VS2022, which has supported /std:c17 since
    # 16.8.
    extra_compile_args += ["/O2", "/std:c17"]


define_macros = []
if USE_LIMITED:
    define_macros.append(("Py_LIMITED_API", hex(PY_LIMITED_API)))

ext = Extension(
    name="lightusd._core",
    sources=[
        "src/python/py-module.c",
        "src/python/py-import.c",
        "src/python/py-physics.c",
        "src/python/py-validation.c",
        "src/python/py-array.c",
        "src/python/py-numeric.c",
        "src/python/py-prim.c",
        "src/python/py-render.c",
        "src/python/py-resolver.c",
        "src/python/py-stage.c",
    ],
    include_dirs=[
        "src",
        "src/c-api",
        "src/python",
    ],
    define_macros=define_macros,
    py_limited_api=USE_LIMITED,
    extra_compile_args=extra_compile_args,
    extra_link_args=extra_link_args,
    language="c",
)


options = {}
if USE_LIMITED:
    options["bdist_wheel"] = {"py_limited_api": "cp311"}

setup(
    ext_modules=[ext],
    cmdclass={"build_ext": CMakeBuildExt},
    options=options,
)

# CLAUDE.md

See [AGENTS.md](AGENTS.md) for full project guidance (structure, build, test, conventions).

## Quick Reference

- build folder @build make with -j16
- wasm build folder @web/build
- native tests (headless, always): `scripts/xctest.sh ctest --test-dir build_ninja -j4 --output-on-failure` (private Xvfb; never run GUI tests on the real display)
- native tests: `cd build && ctest --output-on-failure`
- roundtrip tests: `bash tests/run-usdcat-compare.sh`

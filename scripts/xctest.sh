#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2024-Present Light Transport Entertainment Inc.
#
# Run a command (normally ctest) headless, so GUI/window-creating tests cannot
# open windows on, or steal focus from, the developer's desktop session.
#
# On Linux this starts a private Xvfb (xvfb-run -a) and removes the real
# DISPLAY / WAYLAND_DISPLAY first. Removing WAYLAND_DISPLAY matters: GLFW and
# SDL prefer Wayland and would otherwise ignore the virtual X server.
#
# Usage:
#   scripts/xctest.sh ctest --test-dir build_ninja -j4
#   scripts/xctest.sh ctest --test-dir build_ninja -R '^lusdview' --output-on-failure
#   scripts/xctest.sh build_ninja/lusdview model.usda      # any GUI command
#
# Environment:
#   XCTEST_SCREEN=WxHxD       Xvfb screen (default 1920x1080x24)
#   XCTEST_REAL_DISPLAY=1     Opt out and use the real session (e.g. to run the
#                             GPU-only tests that Mesa llvmpipe skips). Use only
#                             when a human has agreed to windows appearing.
#
# Note: Xvfb provides software GL (llvmpipe) and no Vulkan, so a few lusdview
# tests report SKIP there (skinning/UV vertex-attribute fetch); they need a
# real-GPU display to run.
set -euo pipefail

if [[ $# -eq 0 ]]; then
  echo "usage: $0 <command> [args...]   (e.g. $0 ctest --test-dir build_ninja -j4)" >&2
  exit 2
fi

# Windows/macOS have no Xvfb; nothing to isolate here.
if [[ "$(uname -s)" != "Linux" ]]; then
  exec "$@"
fi

# Already isolated (nested call), or the caller explicitly opted out.
if [[ "${XCTEST_ACTIVE:-0}" == "1" || "${XCTEST_REAL_DISPLAY:-0}" == "1" ]]; then
  exec "$@"
fi

if ! command -v xvfb-run >/dev/null 2>&1; then
  echo "xctest.sh: xvfb-run not found; install xvfb (e.g. 'apt install xvfb')," >&2
  echo "           or set XCTEST_REAL_DISPLAY=1 to run on the real display." >&2
  exit 127
fi

exec env -u WAYLAND_DISPLAY -u DISPLAY \
  XCTEST_ACTIVE=1 XDG_SESSION_TYPE=x11 SDL_VIDEODRIVER=x11 GDK_BACKEND=x11 \
  xvfb-run -a -s "-screen 0 ${XCTEST_SCREEN:-1920x1080x24}" "$@"

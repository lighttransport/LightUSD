#!/bin/sh
# Native dump for tools/view_control/skinning_compare.py: skin_dump.sh SCENE TIME
# SKIN_COMPARE: path to the skin_compare executable (tests/obj-skel/skin_compare.cc).
exec "${SKIN_COMPARE:-skin_compare}" --dump "$1" "$2"

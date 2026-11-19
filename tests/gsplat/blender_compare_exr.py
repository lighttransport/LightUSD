# SPDX-License-Identifier: Apache-2.0
# Blender-Python comparator for scene-linear EXR renders (the system Python
# may lack an EXR reader). Prints one "CMP name mae ratio_r ratio_g ratio_b"
# line per candidate, measured against REF, plus the REF vs REF2 noise floor.
#   blender -b --factory-startup --python blender_compare_exr.py -- \
#       REF.exr REF2.exr CAND1.exr [CAND2.exr ...]
import sys

import bpy
import numpy as np

args = sys.argv[sys.argv.index("--") + 1:]


def load(path):
    im = bpy.data.images.load(path)
    w, h = im.size
    return np.array(im.pixels[:], dtype=np.float64).reshape(h, w, 4)[:, :, :3]


ref = load(args[0])
floor = load(args[1])
mean = ref.mean((0, 1))


def report(name, img):
    ratio = img.mean((0, 1)) / np.maximum(mean, 1e-9)
    print("CMP %s %.6f %.5f %.5f %.5f" %
          (name, np.abs(img - ref).mean(), ratio[0], ratio[1], ratio[2]))


report("floor", floor)
for path in args[2:]:
    report(path, load(path))

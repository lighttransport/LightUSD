# SPDX-License-Identifier: Apache-2.0
"""lightusd.tydra — render-scene extraction (Tydra).

Convert a loaded :class:`lightusd.Stage` into GPU-friendly render data::

    import numpy as np
    import lightusd
    from lightusd import tydra

    stage = lightusd.load("scene.usdz")
    scene = tydra.to_render_scene(stage)
    for mesh in scene.meshes:
        vertices = np.asarray(mesh.points)               # (N, 3) float32
        indices = np.asarray(mesh.triangulated_indices)  # (T,) uint32
"""

from ._core import (  # noqa: F401
    RenderCamera,
    RenderImage,
    RenderLight,
    RenderMaterial,
    RenderMesh,
    RenderNode,
    RenderScene,
    RenderTexture,
    to_render_scene,
)

__all__ = [
    "RenderCamera",
    "RenderImage",
    "RenderLight",
    "RenderMaterial",
    "RenderMesh",
    "RenderNode",
    "RenderScene",
    "RenderTexture",
    "to_render_scene",
]

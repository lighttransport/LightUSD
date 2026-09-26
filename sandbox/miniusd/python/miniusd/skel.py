"""UsdSkel: skeletons, skinning, blend shapes and animation.

Authoring (Blender-compatible layout):

    from miniusd import skel
    root = skel.add_skel_root(stage, "/Character")
    sk = skel.add_skeleton(stage, "/Character/Skel", joints=["Hips", "Hips/Spine"],
                           bind_transforms=[m0, m1])            # world-space (skeleton space)
    anim = skel.add_animation(stage, "/Character/Skel/Anim", joints=["Hips", "Hips/Spine"],
                              rotations={1: [q0, q1], 24: [q0, q2]})
    skel.bind_animation(sk, anim)
    mesh = geom.add_mesh(stage, "/Character/Body", ...)
    skel.bind_skin(mesh, sk, joint_indices, joint_weights, element_size=2)
    skel.add_blendshape(mesh, "Smile", offsets, point_indices)

Evaluation (layer-level, no composition): skel.evaluate_points(stage, mesh_path, time)
applies blend shapes and linear blend skinning on the CPU and returns the deformed
points in skeleton space.

Matrices follow USD's row-vector convention (p' = p * M, translation in row 3) and
quaternions are (w, x, y, z).
"""

import math

from .values import _is_np, flatten, np

IDENTITY = ((1.0, 0.0, 0.0, 0.0), (0.0, 1.0, 0.0, 0.0), (0.0, 0.0, 1.0, 0.0), (0.0, 0.0, 0.0, 1.0))


# ---------------------------------------------------------------------------
# small matrix / quaternion math (pure python, 4x4 row-vector convention)
# ---------------------------------------------------------------------------

def mat_mul(a, b):
    return tuple(tuple(sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)) for i in range(4))


def mat_inverse(m):
    a = [list(map(float, row)) + [1.0 if i == j else 0.0 for j in range(4)] for i, row in enumerate(m)]
    for c in range(4):
        p = max(range(c, 4), key=lambda r: abs(a[r][c]))
        if abs(a[p][c]) < 1e-300:
            raise ValueError("singular matrix")
        a[c], a[p] = a[p], a[c]
        inv = 1.0 / a[c][c]
        a[c] = [x * inv for x in a[c]]
        for r in range(4):
            if r != c and a[r][c]:
                f = a[r][c]
                a[r] = [x - f * y for x, y in zip(a[r], a[c])]
    return tuple(tuple(row[4:]) for row in a)


def quat_to_mat3(q):
    """Unit quaternion (w, x, y, z) -> 3x3 rotation rows (row-vector convention)."""
    w, x, y, z = (float(v) for v in q)
    n = math.sqrt(w * w + x * x + y * y + z * z) or 1.0
    w, x, y, z = w / n, x / n, y / n, z / n
    return ((1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)),
            (2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)),
            (2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)))


def mat3_to_quat(r):
    """3x3 rotation rows -> unit quaternion (w, x, y, z) (inverse of quat_to_mat3)."""
    t = r[0][0] + r[1][1] + r[2][2]
    if t > 0:
        s = math.sqrt(t + 1.0) * 2
        return (0.25 * s, (r[1][2] - r[2][1]) / s, (r[2][0] - r[0][2]) / s, (r[0][1] - r[1][0]) / s)
    if r[0][0] > r[1][1] and r[0][0] > r[2][2]:
        s = math.sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]) * 2
        return ((r[1][2] - r[2][1]) / s, 0.25 * s, (r[1][0] + r[0][1]) / s, (r[2][0] + r[0][2]) / s)
    if r[1][1] > r[2][2]:
        s = math.sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]) * 2
        return ((r[2][0] - r[0][2]) / s, (r[1][0] + r[0][1]) / s, 0.25 * s, (r[2][1] + r[1][2]) / s)
    s = math.sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]) * 2
    return ((r[0][1] - r[1][0]) / s, (r[2][0] + r[0][2]) / s, (r[2][1] + r[1][2]) / s, 0.25 * s)


def quat_from_axis_angle(axis, degrees):
    ax = [float(v) for v in axis]
    n = math.sqrt(sum(v * v for v in ax)) or 1.0
    h = math.radians(degrees) * 0.5
    s = math.sin(h) / n
    return (math.cos(h), ax[0] * s, ax[1] * s, ax[2] * s)


def trs_matrix(translate=(0, 0, 0), rotate=(1, 0, 0, 0), scale=(1, 1, 1)):
    """Local transform = scale * rotate * translate (UsdSkel order)."""
    r = quat_to_mat3(rotate)
    sx, sy, sz = (float(v) for v in scale)
    t = [float(v) for v in translate]
    return (tuple(r[0][j] * sx for j in range(3)) + (0.0,),
            tuple(r[1][j] * sy for j in range(3)) + (0.0,),
            tuple(r[2][j] * sz for j in range(3)) + (0.0,),
            (t[0], t[1], t[2], 1.0))


def decompose_trs(m):
    """Matrix without shear -> (translate, rotate quat, scale)."""
    rows = [list(map(float, m[i][:3])) for i in range(3)]
    scale = [math.sqrt(sum(v * v for v in row)) or 1.0 for row in rows]
    r = [[v / scale[i] for v in rows[i]] for i in range(3)]
    return tuple(float(v) for v in m[3][:3]), mat3_to_quat(r), tuple(scale)


def translation_matrix(t):
    return trs_matrix(translate=t)


def _mats(value):
    """Matrix array (ndarray or list) -> list of 4x4 tuples."""
    if value is None:
        return []
    if _is_np(value):
        value = value.tolist()
    return [tuple(tuple(float(x) for x in row) for row in m) for m in value]


def _rows(value, n):
    if value is None:
        return []
    f = flatten(value)
    return [tuple(f[i:i + n]) for i in range(0, len(f), n)]


def joint_parents(joints):
    """Parent index per joint (-1 for roots) from joint path tokens."""
    index = {j: i for i, j in enumerate(joints)}
    out = []
    for j in joints:
        p = j.rsplit("/", 1)[0] if "/" in j else None
        out.append(index.get(p, -1) if p else -1)
    return out


def local_to_world(joints, local_transforms):
    """Concatenate joint-local transforms down the hierarchy (skeleton space)."""
    parents = joint_parents(joints)
    world = [None] * len(joints)
    for i, m in enumerate(local_transforms):
        p = parents[i]
        world[i] = m if p < 0 else mat_mul(m, world[p])
    return world


def world_to_local(joints, world_transforms):
    parents = joint_parents(joints)
    return [m if parents[i] < 0 else mat_mul(m, mat_inverse(world_transforms[parents[i]]))
            for i, m in enumerate(world_transforms)]


# ---------------------------------------------------------------------------
# authoring
# ---------------------------------------------------------------------------

def _define(parent, path, type_name):
    if hasattr(parent, "root"):
        return parent.define(path, type_name)
    parts = [x for x in str(path).split("/") if x]
    for part in parts[:-1]:
        parent = parent.children.get(part) or parent.define(part)
    return parent.define(parts[-1], type_name)


def add_skel_root(stage, path):
    return _define(stage, path, "SkelRoot")


def add_skeleton(stage, path, joints, bind_transforms=None, rest_transforms=None, joint_names=None):
    """Skeleton prim. `joints` are joint paths ("Hips", "Hips/Spine", ...).
    bind_transforms are skeleton-space (world) matrices; rest_transforms are
    joint-local. Either one is derived from the other when omitted."""
    joints = [str(j) for j in joints]
    if bind_transforms is None and rest_transforms is None:
        bind_transforms = [IDENTITY] * len(joints)
    bind = _mats(bind_transforms) if bind_transforms is not None else \
        local_to_world(joints, _mats(rest_transforms))
    rest = _mats(rest_transforms) if rest_transforms is not None else world_to_local(joints, bind)
    sk = _define(stage, path, "Skeleton")
    sk.apply_api("SkelBindingAPI")
    sk.create_attribute("joints", "token[]", joints, uniform=True)
    sk.create_attribute("bindTransforms", "matrix4d[]", bind, uniform=True)
    sk.create_attribute("restTransforms", "matrix4d[]", rest, uniform=True)
    if joint_names is not None:
        sk.create_attribute("jointNames", "token[]", joint_names, uniform=True)
    return sk


def _set_sampled(prim, name, type_name, value):
    """value is a static array or {time: array} for time samples."""
    if value is None:
        return
    a = prim.create_attribute(name, type_name)
    if isinstance(value, dict):
        for t, v in value.items():
            a.set(v, time=t)
    else:
        a.set(value)


def add_animation(stage, path, joints=None, translations=None, rotations=None, scales=None,
                  blend_shapes=None, blend_shape_weights=None):
    """SkelAnimation. translations/rotations/scales/blend_shape_weights are
    arrays (one entry per joint / blend shape) or {time: array} dicts.
    Rotations are quaternions (w, x, y, z)."""
    an = _define(stage, path, "SkelAnimation")
    if joints is not None:
        an.create_attribute("joints", "token[]", [str(j) for j in joints], uniform=True)
    _set_sampled(an, "translations", "float3[]", translations)
    _set_sampled(an, "rotations", "quatf[]", rotations)
    _set_sampled(an, "scales", "half3[]", scales)
    if blend_shapes is not None:
        an.create_attribute("blendShapes", "token[]", list(blend_shapes), uniform=True)
    _set_sampled(an, "blendShapeWeights", "float[]", blend_shape_weights)
    return an


def bind_animation(skeleton, animation):
    skeleton.apply_api("SkelBindingAPI")
    skeleton.create_relationship("skel:animationSource", [getattr(animation, "path", animation)])
    return skeleton


def bind_skin(mesh, skeleton, joint_indices, joint_weights, element_size=1, geom_bind_transform=None,
              joints=None, constant=False):
    """Bind a mesh to a skeleton with per-vertex joint influences
    (element_size influences per point, flattened). `joints` optionally gives a
    mesh-specific joint order that the indices refer to."""
    mesh.apply_api("SkelBindingAPI")
    interp = "constant" if constant else "vertex"
    mesh.create_attribute("primvars:skel:jointIndices", "int[]", joint_indices,
                          interpolation=interp, elementSize=int(element_size))
    mesh.create_attribute("primvars:skel:jointWeights", "float[]", joint_weights,
                          interpolation=interp, elementSize=int(element_size))
    if geom_bind_transform is not None:
        mesh.create_attribute("primvars:skel:geomBindTransform", "matrix4d", geom_bind_transform)
    if joints is not None:
        mesh.create_attribute("skel:joints", "token[]", list(joints), uniform=True)
    mesh.create_relationship("skel:skeleton", [getattr(skeleton, "path", skeleton)])
    return mesh


def add_blendshape(mesh, name, offsets, point_indices=None, normal_offsets=None, inbetweens=None):
    """Add a BlendShape child to `mesh` and register it in skel:blendShapes /
    skel:blendShapeTargets. inbetweens: {name: (weight, offsets[, normal_offsets])}."""
    bs = mesh.define(name, "BlendShape")
    bs.create_attribute("offsets", "vector3f[]", offsets, uniform=True)
    if point_indices is not None:
        bs.create_attribute("pointIndices", "int[]", point_indices, uniform=True)
    if normal_offsets is not None:
        bs.create_attribute("normalOffsets", "vector3f[]", normal_offsets, uniform=True)
    for ib_name, ib in (inbetweens or {}).items():
        bs.create_attribute("inbetweens:" + ib_name, "vector3f[]", ib[1], uniform=True, weight=float(ib[0]))
        if len(ib) > 2 and ib[2] is not None:
            bs.create_attribute("inbetweens:%s:normalOffsets" % ib_name, "vector3f[]", ib[2], uniform=True)
    mesh.apply_api("SkelBindingAPI")
    names = list(mesh.get("skel:blendShapes", default=[]))
    if name not in names:
        names.append(name)
    mesh.create_attribute("skel:blendShapes", "token[]", names, uniform=True)
    rel = mesh.create_relationship("skel:blendShapeTargets")
    targets = rel.get_targets()
    if bs.path not in targets:
        rel.set_targets(*(targets + [bs.path]))
    return bs


def skin_weights_from_nearest(points, joint_positions, element_size=2, falloff=2.0):
    """Utility for quick rigs: inverse-distance weights to the `element_size`
    nearest joints. Returns (joint_indices, joint_weights) flattened."""
    pts = _rows(points, 3)
    jp = _rows(joint_positions, 3)
    idx, wts = [], []
    for p in pts:
        d = sorted((math.dist(p, j), i) for i, j in enumerate(jp))[:element_size]
        w = [1.0 / (max(dd, 1e-6) ** falloff) for dd, _ in d]
        s = sum(w)
        while len(d) < element_size:
            d.append((0.0, 0))
            w.append(0.0)
        idx += [i for _, i in d]
        wts += [x / s for x in w]
    return idx, wts


# ---------------------------------------------------------------------------
# evaluation
# ---------------------------------------------------------------------------

def _find_rel(stage, prim, name):
    """Binding relationships are inherited down namespace (SkelBindingAPI)."""
    p = prim
    while p is not None:
        r = p.relationship(name)
        if r is not None and r.get_targets():
            return stage.prim_at(r.get_targets()[0])
        p = p.parent
    return None


def _anim_value(anim, name, time):
    a = anim.attribute(name) if anim is not None else None
    return None if a is None else a.get(time)


def joint_local_transforms(skeleton, animation=None, time=None):
    """Joint-local matrices: rest transforms overridden by the animation."""
    joints = list(skeleton.get("joints", default=[]))
    rest = _mats(skeleton.get("restTransforms"))
    if len(rest) != len(joints):
        rest = world_to_local(joints, _mats(skeleton.get("bindTransforms")) or [IDENTITY] * len(joints))
    if animation is None:
        return joints, rest
    ajoints = list(animation.get("joints", default=[]))
    t = _rows(_anim_value(animation, "translations", time), 3)
    r = _rows(_anim_value(animation, "rotations", time), 4)
    s = _rows(_anim_value(animation, "scales", time), 3)
    index = {j: i for i, j in enumerate(joints)}
    local = list(rest)
    for ai, j in enumerate(ajoints):
        i = index.get(j)
        if i is None:
            continue
        rt, rr, rs = decompose_trs(rest[i])
        local[i] = trs_matrix(t[ai] if ai < len(t) else rt, r[ai] if ai < len(r) else rr,
                              s[ai] if ai < len(s) else rs)
    return joints, local


def skinning_transforms(skeleton, animation=None, time=None):
    """Per-joint skinning matrix = inverse(bind) * animated skeleton-space xform."""
    joints, local = joint_local_transforms(skeleton, animation, time)
    world = local_to_world(joints, local)
    bind = _mats(skeleton.get("bindTransforms")) or world
    return joints, [mat_mul(mat_inverse(b), w) for b, w in zip(bind, world)]


def apply_blendshapes(points, shapes, weights):
    """points + sum(weight * offsets) (shapes: [(offsets, point_indices or None)])."""
    pts = [list(p) for p in _rows(points, 3)]
    for (offsets, pidx), w in zip(shapes, weights):
        w = float(w)
        if not w:
            continue
        off = _rows(offsets, 3)
        ids = list(flatten(pidx)) if pidx is not None else range(len(off))
        for k, i in enumerate(ids):
            p = pts[int(i)]
            o = off[k]
            p[0] += w * o[0]
            p[1] += w * o[1]
            p[2] += w * o[2]
    return pts


def skin_points(points, joint_indices, joint_weights, element_size, xforms, geom_bind=None,
                constant=False):
    """Linear blend skinning. Returns a list of (x, y, z) (or ndarray with numpy)."""
    pts = _rows(points, 3)
    ji = [int(v) for v in flatten(joint_indices)]
    jw = [float(v) for v in flatten(joint_weights)]
    n = element_size
    if np is not None:
        P = np.asarray(pts, dtype=np.float64).reshape(-1, 3)
        P = np.hstack([P, np.ones((len(P), 1))])
        if geom_bind is not None:
            P = P @ np.asarray(geom_bind, dtype=np.float64)
        X = np.asarray(xforms, dtype=np.float64).reshape(-1, 4, 4)
        I = np.asarray(ji, dtype=np.int64).reshape(-1, n)
        W = np.asarray(jw, dtype=np.float64).reshape(-1, n)
        if constant:
            I = np.repeat(I[:1], len(P), axis=0)
            W = np.repeat(W[:1], len(P), axis=0)
        out = np.zeros((len(P), 4))
        for k in range(n):
            out += W[:, k:k + 1] * np.einsum("pi,pij->pj", P, X[I[:, k]])
        return out[:, :3]
    out = []
    for pi, p in enumerate(pts):
        v = (float(p[0]), float(p[1]), float(p[2]), 1.0)
        if geom_bind is not None:
            v = tuple(sum(v[k] * geom_bind[k][j] for k in range(4)) for j in range(4))
        base = 0 if constant else pi * n
        acc = [0.0, 0.0, 0.0]
        for k in range(n):
            w = jw[base + k]
            if not w:
                continue
            m = xforms[ji[base + k]]
            for j in range(3):
                acc[j] += w * sum(v[r] * m[r][j] for r in range(4))
        out.append(tuple(acc))
    return out


def evaluate_points(stage, mesh, time=None):
    """Deformed points of a skinned / blendshaped mesh at `time` (skeleton space
    when skinned). Layer-level: all targets must be in this layer."""
    mesh = stage.prim_at(mesh) if isinstance(mesh, str) else mesh
    points = mesh.get("points", time)
    skeleton = _find_rel(stage, mesh, "skel:skeleton")
    anim = _find_rel(stage, skeleton, "skel:animationSource") if skeleton is not None else None

    names = list(mesh.get("skel:blendShapes", default=[]))
    rel = mesh.relationship("skel:blendShapeTargets")
    if names and rel is not None and anim is not None:
        abs_names = list(anim.get("blendShapes", default=[]))
        aw = [float(x) for x in flatten(_anim_value(anim, "blendShapeWeights", time) or [])]
        wmap = dict(zip(abs_names, aw))
        shapes, weights = [], []
        for nm, tgt in zip(names, rel.get_targets()):
            bs = stage.prim_at(tgt)
            if bs is not None and nm in wmap:
                shapes.append((bs.get("offsets"), bs.get("pointIndices")))
                weights.append(wmap[nm])
        points = apply_blendshapes(points, shapes, weights)

    ji_attr = mesh.attribute("primvars:skel:jointIndices")
    if skeleton is None or ji_attr is None:
        return points
    sk_joints, xforms = skinning_transforms(skeleton, anim, time)
    mesh_joints = mesh.get("skel:joints")
    if mesh_joints is not None:  # remap mesh-local joint order to skeleton order
        idx = {j: i for i, j in enumerate(sk_joints)}
        xforms = [xforms[idx[j]] if j in idx else IDENTITY for j in mesh_joints]
    es = int(ji_attr.metadata.get("elementSize", 1))
    gb = mesh.get("primvars:skel:geomBindTransform")
    return skin_points(points, ji_attr.get(), mesh.get("primvars:skel:jointWeights"), es, xforms,
                       gb, ji_attr.metadata.get("interpolation") == "constant")


def read_skeleton(skeleton):
    """Plain-data summary of a Skeleton prim."""
    joints = list(skeleton.get("joints", default=[]))
    return {"joints": joints, "parents": joint_parents(joints),
            "bind_transforms": _mats(skeleton.get("bindTransforms")),
            "rest_transforms": _mats(skeleton.get("restTransforms"))}

/* SPDX-License-Identifier: Apache-2.0
 * lightusd._core — tydra render-scene binding (RenderScene + item facades).
 */

#include "py-internal.h"
#include "lightusd-render-c.h"

/* Additional state fields live in lightusd_state (see py-internal.h). */

typedef struct {
  PyObject_HEAD
  lightusd_render_scene* scene; /* owned */
} LightUSDRenderScene;

typedef struct {
  PyObject_HEAD
  PyObject* scene; /* strong ref to LightUSDRenderScene */
  int32_t id;
} LightUSDRenderItem; /* shared shape for all per-id facades */

static lightusd_render_scene* item_scene(LightUSDRenderItem* it) {
  return ((LightUSDRenderScene*)it->scene)->scene;
}

/* Wrap a render buffer view as a zero-copy Array anchored to the scene. */
static PyObject* wrap_buffer(lightusd_state* st, PyObject* scene_obj,
                             const lightusd_buffer_view* buf) {
  if (!buf->data || buf->count == 0) Py_RETURN_NONE;
  lightusd_buffer* pin = NULL;
  lightusd_status status = lightusd_render_buffer_pin(((LightUSDRenderScene*)scene_obj)->scene, buf, &pin);
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, "pin render buffer");
  return lightusd_wrap_array_pin(st, pin);
}

static PyObject* matrix16f_to_python(const float m[16]) {
  PyObject* rows = PyTuple_New(4);
  if (!rows) return NULL;
  for (int r = 0; r < 4; ++r) {
    PyObject* row = Py_BuildValue(
        "(dddd)", (double)m[r * 4 + 0], (double)m[r * 4 + 1],
        (double)m[r * 4 + 2], (double)m[r * 4 + 3]);
    if (!row) {
      Py_DECREF(rows);
      return NULL;
    }
    PyTuple_SetItem(rows, r, row);
  }
  return rows;
}

/* Generic item alloc */
static PyObject* item_new(lightusd_state* st, PyObject* type_obj,
                          PyObject* scene_obj, int32_t id) {
  (void)st;
  PyObject* obj = lightusd_alloc(type_obj);
  if (!obj) return NULL;
  LightUSDRenderItem* it = (LightUSDRenderItem*)obj;
  it->scene = scene_obj;
  Py_INCREF(scene_obj);
  it->id = id;
  return obj;
}

static void item_dealloc(PyObject* self) {
  LightUSDRenderItem* it = (LightUSDRenderItem*)self;
  PyTypeObject* tp = Py_TYPE(self);
  Py_CLEAR(it->scene);
  freefunc free_fn = (freefunc)PyType_GetSlot(tp, Py_tp_free);
  free_fn(self);
  Py_DECREF((PyObject*)tp);
}

/* ============================================================
 * RenderMesh
 * ============================================================ */

static int mesh_info(LightUSDRenderItem* it, lightusd_render_mesh_info* info) {
  lightusd_state* st = lightusd_state_from_obj((PyObject*)it);
  if (!st) return -1;
  lightusd_status s = lightusd_render_mesh_get_info(item_scene(it), it->id, info);
  if (s != LIGHTUSD_OK) {
    lightusd_raise(st, s, "render mesh");
    return -1;
  }
  return 0;
}

static PyObject* RMesh_get_str(PyObject* self, void* closure) {
  lightusd_render_mesh_info info;
  if (mesh_info((LightUSDRenderItem*)self, &info) != 0) return NULL;
  return lightusd_sv_to_str((uintptr_t)closure == 0 ? info.name : info.prim_path);
}

static PyObject* RMesh_get_int(PyObject* self, void* closure) {
  lightusd_render_mesh_info info;
  if (mesh_info((LightUSDRenderItem*)self, &info) != 0) return NULL;
  switch ((uintptr_t)closure) {
    case 0:
      return PyLong_FromUnsignedLongLong(info.point_count);
    case 1:
      return PyLong_FromUnsignedLongLong(info.face_count);
    case 2:
      return PyLong_FromLong(info.material_id);
    case 3:
      return PyLong_FromUnsignedLong(info.primvar_count);
    case 4:
      return PyLong_FromUnsignedLong(info.blend_shape_count);
    case 5:
      return PyLong_FromLong(info.skeleton_id);
    default:
      Py_RETURN_NONE;
  }
}

static PyObject* RMesh_get_bool(PyObject* self, void* closure) {
  lightusd_render_mesh_info info;
  if (mesh_info((LightUSDRenderItem*)self, &info) != 0) return NULL;
  switch ((uintptr_t)closure) {
    case 0:
      return PyBool_FromLong(info.is_triangulated);
    case 1:
      return PyBool_FromLong(info.has_skin);
    default:
      Py_RETURN_NONE;
  }
}

static PyObject* RMesh_get_buffer(PyObject* self, void* closure) {
  LightUSDRenderItem* it = (LightUSDRenderItem*)self;
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;
  lightusd_buffer_view buf;
  lightusd_status s = lightusd_render_mesh_buffer(item_scene(it), it->id,
                                          (uint8_t)(uintptr_t)closure, &buf);
  if (s == LIGHTUSD_ERR_NOT_FOUND) Py_RETURN_NONE;
  if (s != LIGHTUSD_OK) return lightusd_raise(st, s, "mesh buffer");
  return wrap_buffer(st, it->scene, &buf);
}

static PyObject* RMesh_get_bbox(PyObject* self, void* closure) {
  (void)closure;
  lightusd_render_mesh_info info;
  if (mesh_info((LightUSDRenderItem*)self, &info) != 0) return NULL;
  if (!info.has_bbox) Py_RETURN_NONE;
  return Py_BuildValue(
      "((fff)(fff))", info.bbox_min[0], info.bbox_min[1], info.bbox_min[2],
      info.bbox_max[0], info.bbox_max[1], info.bbox_max[2]);
}

static PyObject* RMesh_get_subsets(PyObject* self, void* closure) {
  (void)closure;
  LightUSDRenderItem* it = (LightUSDRenderItem*)self;
  lightusd_render_mesh_info info;
  if (mesh_info(it, &info) != 0) return NULL;
  PyObject* list = PyList_New((Py_ssize_t)info.subset_count);
  if (!list) return NULL;
  for (uint32_t i = 0; i < info.subset_count; ++i) {
    uint32_t start = 0, count = 0;
    int32_t mat = -1;
    lightusd_render_mesh_subset(item_scene(it), it->id, i, &start, &count, &mat);
    PyObject* t = Py_BuildValue("(IIi)", start, count, mat);
    if (!t) {
      Py_DECREF(list);
      return NULL;
    }
    PyList_SetItem(list, (Py_ssize_t)i, t);
  }
  return list;
}

static PyObject* RMesh_primvar(PyObject* self, PyObject* args) {
  Py_ssize_t index;
  if (!PyArg_ParseTuple(args, "n:primvar", &index)) return NULL;
  LightUSDRenderItem* it = (LightUSDRenderItem*)self;
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;

  lightusd_render_primvar_info info;
  lightusd_status s = lightusd_render_mesh_primvar_info(item_scene(it), it->id,
                                                (size_t)index, &info);
  if (s != LIGHTUSD_OK) return lightusd_raise(st, s, "primvar");

  lightusd_buffer_view data_buf;
  s = lightusd_render_mesh_primvar_buffer(item_scene(it), it->id, (size_t)index,
                                      0, &data_buf);
  if (s != LIGHTUSD_OK) return lightusd_raise(st, s, "primvar data");
  PyObject* data = wrap_buffer(st, it->scene, &data_buf);
  if (!data) return NULL;

  PyObject* indices = Py_None;
  Py_INCREF(Py_None);
  if (info.has_indices) {
    lightusd_buffer_view idx_buf;
    if (lightusd_render_mesh_primvar_buffer(item_scene(it), it->id,
                                        (size_t)index, 1,
                                        &idx_buf) == LIGHTUSD_OK) {
      Py_DECREF(indices);
      indices = wrap_buffer(st, it->scene, &idx_buf);
      if (!indices) {
        Py_DECREF(data);
        return NULL;
      }
    }
  }
  static const char* interp_names[] = {"constant", "uniform", "vertex",
                                       "facevarying", "varying"};
  const char* interp =
      info.interpolation <= 4 ? interp_names[info.interpolation] : "vertex";
  PyObject* name = lightusd_sv_to_str(info.name);
  if (!name) {
    Py_DECREF(data);
    Py_DECREF(indices);
    return NULL;
  }
  return Py_BuildValue("{s:N,s:s,s:N,s:N}", "name", name, "interpolation",
                       interp, "data", data, "indices", indices);
}

static PyObject* RMesh_blend_shape(PyObject* self, PyObject* args) {
  Py_ssize_t index;
  if (!PyArg_ParseTuple(args, "n:blend_shape", &index)) return NULL;
  LightUSDRenderItem* it = (LightUSDRenderItem*)self;
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;
  lightusd_sv name;
  float weight = 0.0f;
  lightusd_buffer_view points_buf;
  lightusd_status s = lightusd_render_mesh_blendshape(
      item_scene(it), it->id, (size_t)index, 0, &name, &weight, &points_buf);
  if (s != LIGHTUSD_OK) return lightusd_raise(st, s, "blend shape");
  PyObject* points = wrap_buffer(st, it->scene, &points_buf);
  if (!points) return NULL;
  lightusd_buffer_view normals_buf;
  PyObject* normals = Py_None;
  Py_INCREF(Py_None);
  if (lightusd_render_mesh_blendshape(item_scene(it), it->id, (size_t)index, 1,
                                  NULL, NULL, &normals_buf) == LIGHTUSD_OK) {
    Py_DECREF(normals);
    normals = wrap_buffer(st, it->scene, &normals_buf);
    if (!normals) {
      Py_DECREF(points);
      return NULL;
    }
  }
  PyObject* pyname = lightusd_sv_to_str(name);
  if (!pyname) {
    Py_DECREF(points);
    Py_DECREF(normals);
    return NULL;
  }
  return Py_BuildValue("{s:N,s:f,s:N,s:N}", "name", pyname, "weight", weight,
                       "point_offsets", points, "normal_offsets", normals);
}

static PyObject* RMesh_repr(PyObject* self) {
  lightusd_render_mesh_info info;
  if (mesh_info((LightUSDRenderItem*)self, &info) != 0) {
    PyErr_Clear();
    return PyUnicode_FromString("RenderMesh(<invalid>)");
  }
  return PyUnicode_FromFormat(
      "RenderMesh('%.*s', points=%llu, faces=%llu)", (int)info.prim_path.len,
      info.prim_path.data, (unsigned long long)info.point_count,
      (unsigned long long)info.face_count);
}

static PyMethodDef RMesh_methods[] = {
    {"primvar", RMesh_primvar, METH_VARARGS,
     "primvar(index) -> {name, interpolation, data, indices}"},
    {"blend_shape", RMesh_blend_shape, METH_VARARGS,
     "blend_shape(index) -> {name, weight, point_offsets, normal_offsets}"},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef RMesh_getset[] = {
    {"name", RMesh_get_str, NULL, NULL, (void*)(uintptr_t)0},
    {"prim_path", RMesh_get_str, NULL, NULL, (void*)(uintptr_t)1},
    {"point_count", RMesh_get_int, NULL, NULL, (void*)(uintptr_t)0},
    {"face_count", RMesh_get_int, NULL, NULL, (void*)(uintptr_t)1},
    {"material_id", RMesh_get_int, NULL, NULL, (void*)(uintptr_t)2},
    {"primvar_count", RMesh_get_int, NULL, NULL, (void*)(uintptr_t)3},
    {"blend_shape_count", RMesh_get_int, NULL, NULL, (void*)(uintptr_t)4},
    {"skeleton_id", RMesh_get_int, NULL, NULL, (void*)(uintptr_t)5},
    {"is_triangulated", RMesh_get_bool, NULL, NULL, (void*)(uintptr_t)0},
    {"has_skin", RMesh_get_bool, NULL, NULL, (void*)(uintptr_t)1},
    {"points", RMesh_get_buffer, NULL, "float32 (N,3)",
     (void*)(uintptr_t)LIGHTUSD_MESH_BUF_POINTS},
    {"face_vertex_counts", RMesh_get_buffer, NULL, "uint32 (F,)",
     (void*)(uintptr_t)LIGHTUSD_MESH_BUF_FACE_COUNTS},
    {"face_vertex_indices", RMesh_get_buffer, NULL, "uint32 (I,)",
     (void*)(uintptr_t)LIGHTUSD_MESH_BUF_FACE_INDICES},
    {"triangulated_indices", RMesh_get_buffer, NULL, "uint32 (T,)",
     (void*)(uintptr_t)LIGHTUSD_MESH_BUF_TRI_INDICES},
    {"triangulated_face_vertex_indices", RMesh_get_buffer, NULL,
     "uint32 (T,) faceVarying corner remap or None",
     (void*)(uintptr_t)LIGHTUSD_MESH_BUF_TRI_FACEVARYING_INDICES},
    {"normals", RMesh_get_buffer, NULL, "float32 (N,3) or None",
     (void*)(uintptr_t)LIGHTUSD_MESH_BUF_NORMALS},
    {"tangents", RMesh_get_buffer, NULL, "float32 (N,4) or None",
     (void*)(uintptr_t)LIGHTUSD_MESH_BUF_TANGENTS},
    {"texcoords0", RMesh_get_buffer, NULL, "float32 (N,2) or None",
     (void*)(uintptr_t)LIGHTUSD_MESH_BUF_TEXCOORDS0},
    {"texcoords1", RMesh_get_buffer, NULL, "float32 (N,2) or None",
     (void*)(uintptr_t)LIGHTUSD_MESH_BUF_TEXCOORDS1},
    {"colors", RMesh_get_buffer, NULL, "float32 (N,3) or None",
     (void*)(uintptr_t)LIGHTUSD_MESH_BUF_COLORS},
    {"joint_indices", RMesh_get_buffer, NULL, "uint16 (N,4) or None",
     (void*)(uintptr_t)LIGHTUSD_MESH_BUF_JOINT_INDICES},
    {"joint_weights", RMesh_get_buffer, NULL, "float32 (N,4) or None",
     (void*)(uintptr_t)LIGHTUSD_MESH_BUF_JOINT_WEIGHTS},
    {"bbox", RMesh_get_bbox, NULL, "((min),(max)) or None", NULL},
    {"subsets", RMesh_get_subsets, NULL,
     "GeomSubset list of (face_start, face_count, material_id)", NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

/* ============================================================
 * RenderMaterial
 * ============================================================ */

static PyObject* RMat_get(PyObject* self, void* closure) {
  LightUSDRenderItem* it = (LightUSDRenderItem*)self;
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;
  lightusd_render_material_info info;
  lightusd_status s = lightusd_render_material_get_info(item_scene(it), it->id, &info);
  if (s != LIGHTUSD_OK) return lightusd_raise(st, s, "render material");
  switch ((uintptr_t)closure) {
    case 0:
      return lightusd_sv_to_str(info.name);
    case 1:
      return lightusd_sv_to_str(info.prim_path);
    case 2: {
      static const char* names[] = {NULL, "preview_surface", "openpbr"};
      if (info.shader_type == 0 || info.shader_type > 2) Py_RETURN_NONE;
      return PyUnicode_FromString(names[info.shader_type]);
    }
    case 3:
      return PyBool_FromLong(info.double_sided);
    case 4: {
      static const char* names[] = {"opaque", "mask", "blend"};
      return PyUnicode_FromString(info.alpha_mode <= 2
                                      ? names[info.alpha_mode]
                                      : "opaque");
    }
    case 5:
      return PyFloat_FromDouble((double)info.alpha_cutoff);
    default:
      Py_RETURN_NONE;
  }
}

/* Forward decl: texture wrapping needs the scene facade below. */
static PyObject* render_item_for(lightusd_state* st, PyObject* scene_obj,
                                 PyObject* type_obj, int32_t id);

static PyObject* RMat_param(PyObject* self, PyObject* args) {
  const char* name;
  if (!PyArg_ParseTuple(args, "s:param", &name)) return NULL;
  LightUSDRenderItem* it = (LightUSDRenderItem*)self;
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;
  int32_t texture_id = -1;
  float value[4];
  lightusd_status s = lightusd_render_material_param(item_scene(it), it->id, name,
                                             &texture_id, value);
  if (s == LIGHTUSD_ERR_NOT_FOUND) Py_RETURN_NONE;
  if (s != LIGHTUSD_OK) return lightusd_raise(st, s, name);
  if (texture_id >= 0) {
    return render_item_for(st, it->scene, st->RenderTextureType, texture_id);
  }
  return Py_BuildValue("(ffff)", value[0], value[1], value[2], value[3]);
}

static PyObject* RMat_repr(PyObject* self) {
  PyObject* shader = RMat_get(self, (void*)(uintptr_t)2);
  if (!shader) {
    PyErr_Clear();
    return PyUnicode_FromString("RenderMaterial(<invalid>)");
  }
  PyObject* res = PyUnicode_FromFormat("RenderMaterial(shader=%R)", shader);
  Py_DECREF(shader);
  return res;
}

static PyMethodDef RMat_methods[] = {
    {"param", RMat_param, METH_VARARGS,
     "param(name) -> (r,g,b,a) | RenderTexture | None"},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef RMat_getset[] = {
    {"name", RMat_get, NULL, NULL, (void*)(uintptr_t)0},
    {"prim_path", RMat_get, NULL, NULL, (void*)(uintptr_t)1},
    {"shader", RMat_get, NULL, "'preview_surface' | 'openpbr' | None",
     (void*)(uintptr_t)2},
    {"double_sided", RMat_get, NULL, NULL, (void*)(uintptr_t)3},
    {"alpha_mode", RMat_get, NULL, "'opaque' | 'mask' | 'blend'",
     (void*)(uintptr_t)4},
    {"alpha_cutoff", RMat_get, NULL, NULL, (void*)(uintptr_t)5},
    {NULL, NULL, NULL, NULL, NULL},
};

/* ============================================================
 * RenderTexture / RenderImage
 * ============================================================ */

static PyObject* RTex_get(PyObject* self, void* closure) {
  LightUSDRenderItem* it = (LightUSDRenderItem*)self;
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;
  lightusd_render_texture_info info;
  lightusd_status s = lightusd_render_texture_get_info(item_scene(it), it->id, &info);
  if (s != LIGHTUSD_OK) return lightusd_raise(st, s, "render texture");
  static const char* wrap_names[] = {"repeat", "clamp", "mirror", "black"};
  switch ((uintptr_t)closure) {
    case 0:
      return lightusd_sv_to_str(info.name);
    case 1:
      return lightusd_sv_to_str(info.asset_path);
    case 2:
      return PyLong_FromLong(info.image_id);
    case 3:
      return PyUnicode_FromString(
          info.wrap_s <= 3 ? wrap_names[info.wrap_s] : "repeat");
    case 4:
      return PyUnicode_FromString(
          info.wrap_t <= 3 ? wrap_names[info.wrap_t] : "repeat");
    case 5:
      return Py_BuildValue("(ff)", info.uv_offset[0], info.uv_offset[1]);
    case 6:
      return Py_BuildValue("(ff)", info.uv_scale[0], info.uv_scale[1]);
    case 7:
      return PyFloat_FromDouble((double)info.uv_rotation);
    default:
      Py_RETURN_NONE;
  }
}

static PyObject* RTex_get_image(PyObject* self, void* closure) {
  (void)closure;
  LightUSDRenderItem* it = (LightUSDRenderItem*)self;
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;
  lightusd_render_texture_info info;
  lightusd_status s = lightusd_render_texture_get_info(item_scene(it), it->id, &info);
  if (s != LIGHTUSD_OK) return lightusd_raise(st, s, "render texture");
  if (info.image_id < 0) Py_RETURN_NONE;
  return render_item_for(st, it->scene, st->RenderImageType, info.image_id);
}

static PyObject* RTex_repr(PyObject* self) {
  PyObject* asset = RTex_get(self, (void*)(uintptr_t)1);
  if (!asset) {
    PyErr_Clear();
    return PyUnicode_FromString("RenderTexture(<invalid>)");
  }
  PyObject* res = PyUnicode_FromFormat("RenderTexture(asset=%R)", asset);
  Py_DECREF(asset);
  return res;
}

static PyGetSetDef RTex_getset[] = {
    {"name", RTex_get, NULL, NULL, (void*)(uintptr_t)0},
    {"asset_path", RTex_get, NULL, NULL, (void*)(uintptr_t)1},
    {"image_id", RTex_get, NULL, NULL, (void*)(uintptr_t)2},
    {"wrap_s", RTex_get, NULL, NULL, (void*)(uintptr_t)3},
    {"wrap_t", RTex_get, NULL, NULL, (void*)(uintptr_t)4},
    {"uv_offset", RTex_get, NULL, NULL, (void*)(uintptr_t)5},
    {"uv_scale", RTex_get, NULL, NULL, (void*)(uintptr_t)6},
    {"uv_rotation", RTex_get, NULL, NULL, (void*)(uintptr_t)7},
    {"image", RTex_get_image, NULL, "RenderImage or None", NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyObject* RImg_get(PyObject* self, void* closure) {
  LightUSDRenderItem* it = (LightUSDRenderItem*)self;
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;
  lightusd_render_image_info info;
  lightusd_status s = lightusd_render_image_get_info(item_scene(it), it->id, &info);
  if (s != LIGHTUSD_OK) return lightusd_raise(st, s, "render image");
  switch ((uintptr_t)closure) {
    case 0:
      return lightusd_sv_to_str(info.name);
    case 1:
      return lightusd_sv_to_str(info.resolved_path);
    case 2:
      return PyLong_FromUnsignedLong(info.width);
    case 3:
      return PyLong_FromUnsignedLong(info.height);
    case 4:
      return PyLong_FromLong(info.channels);
    case 5:
      return PyBool_FromLong(info.is_loaded);
    default:
      Py_RETURN_NONE;
  }
}

static PyObject* RImg_get_data(PyObject* self, void* closure) {
  (void)closure;
  LightUSDRenderItem* it = (LightUSDRenderItem*)self;
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;
  lightusd_buffer_view buf;
  lightusd_status s = lightusd_render_image_buffer(item_scene(it), it->id, &buf);
  if (s != LIGHTUSD_OK) return lightusd_raise(st, s, "image data");
  return wrap_buffer(st, it->scene, &buf);
}

static PyObject* RImg_repr(PyObject* self) {
  lightusd_render_image_info info;
  LightUSDRenderItem* it = (LightUSDRenderItem*)self;
  if (lightusd_render_image_get_info(item_scene(it), it->id, &info) != LIGHTUSD_OK) {
    return PyUnicode_FromString("RenderImage(<invalid>)");
  }
  return PyUnicode_FromFormat("RenderImage(%ux%ux%u)", info.width,
                              info.height, (unsigned)info.channels);
}

static PyGetSetDef RImg_getset[] = {
    {"name", RImg_get, NULL, NULL, (void*)(uintptr_t)0},
    {"resolved_path", RImg_get, NULL, NULL, (void*)(uintptr_t)1},
    {"width", RImg_get, NULL, NULL, (void*)(uintptr_t)2},
    {"height", RImg_get, NULL, NULL, (void*)(uintptr_t)3},
    {"channels", RImg_get, NULL, NULL, (void*)(uintptr_t)4},
    {"is_loaded", RImg_get, NULL, NULL, (void*)(uintptr_t)5},
    {"data", RImg_get_data, NULL,
     "uint8 (pixels, channels) Array or None when not loaded", NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

/* ============================================================
 * RenderNode / RenderLight / RenderCamera
 * ============================================================ */

static PyObject* RNode_get(PyObject* self, void* closure) {
  LightUSDRenderItem* it = (LightUSDRenderItem*)self;
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;
  lightusd_render_node_info info;
  lightusd_status s = lightusd_render_node_get_info(item_scene(it), it->id, &info);
  if (s != LIGHTUSD_OK) return lightusd_raise(st, s, "render node");
  static const char* type_names[] = {
      "xform", "mesh", "point_instancer", "camera",
      "point_light", "directional_light", "spot_light", "rect_light",
      "disk_light", "dome_light", "sphere_light", "skeleton"};
  switch ((uintptr_t)closure) {
    case 0:
      return lightusd_sv_to_str(info.name);
    case 1:
      return lightusd_sv_to_str(info.prim_path);
    case 2:
      return PyUnicode_FromString(info.type <= 11 ? type_names[info.type]
                                                  : "xform");
    case 3:
      return PyBool_FromLong(info.visible);
    case 4:
      return PyLong_FromLong(info.data_id);
    case 5:
      return PyLong_FromLong(info.parent_id);
    case 6:
      return matrix16f_to_python(info.local_transform);
    case 7:
      return matrix16f_to_python(info.world_transform);
    default:
      Py_RETURN_NONE;
  }
}

static PyObject* RNode_get_children(PyObject* self, void* closure) {
  (void)closure;
  LightUSDRenderItem* it = (LightUSDRenderItem*)self;
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;
  size_t n = lightusd_render_node_children(item_scene(it), it->id, NULL, 0);
  size_t ids_n = n ? n : 1;
  if (ids_n > SIZE_MAX / sizeof(int32_t)) return PyErr_NoMemory();
  int32_t* ids = (int32_t*)PyMem_Malloc(ids_n * sizeof(int32_t));
  if (!ids) return PyErr_NoMemory();
  lightusd_render_node_children(item_scene(it), it->id, ids, n);
  PyObject* tup = PyTuple_New((Py_ssize_t)n);
  if (!tup) {
    PyMem_Free(ids);
    return NULL;
  }
  for (size_t i = 0; i < n; ++i) {
    PyObject* child =
        render_item_for(st, it->scene, st->RenderNodeType, ids[i]);
    if (!child) {
      PyMem_Free(ids);
      Py_DECREF(tup);
      return NULL;
    }
    PyTuple_SetItem(tup, (Py_ssize_t)i, child);
  }
  PyMem_Free(ids);
  return tup;
}

static PyObject* RNode_repr(PyObject* self) {
  PyObject* path = RNode_get(self, (void*)(uintptr_t)1);
  PyObject* type = RNode_get(self, (void*)(uintptr_t)2);
  if (!path || !type) {
    PyErr_Clear();
    Py_XDECREF(path);
    Py_XDECREF(type);
    return PyUnicode_FromString("RenderNode(<invalid>)");
  }
  PyObject* res =
      PyUnicode_FromFormat("RenderNode(%R, type=%R)", path, type);
  Py_DECREF(path);
  Py_DECREF(type);
  return res;
}

static PyGetSetDef RNode_getset[] = {
    {"name", RNode_get, NULL, NULL, (void*)(uintptr_t)0},
    {"prim_path", RNode_get, NULL, NULL, (void*)(uintptr_t)1},
    {"type", RNode_get, NULL, NULL, (void*)(uintptr_t)2},
    {"visible", RNode_get, NULL, NULL, (void*)(uintptr_t)3},
    {"data_id", RNode_get, NULL, NULL, (void*)(uintptr_t)4},
    {"parent_id", RNode_get, NULL, NULL, (void*)(uintptr_t)5},
    {"local_transform", RNode_get, NULL, NULL, (void*)(uintptr_t)6},
    {"world_transform", RNode_get, NULL, NULL, (void*)(uintptr_t)7},
    {"children", RNode_get_children, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyObject* RLight_get(PyObject* self, void* closure) {
  LightUSDRenderItem* it = (LightUSDRenderItem*)self;
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;
  lightusd_render_light_info info;
  lightusd_status s = lightusd_render_light_get_info(item_scene(it), it->id, &info);
  if (s != LIGHTUSD_OK) return lightusd_raise(st, s, "render light");
  static const char* type_names[] = {"point", "directional", "spot", "rect",
                                     "disk", "dome", "sphere", "cylinder",
                                     "geometry"};
  switch ((uintptr_t)closure) {
    case 0:
      return lightusd_sv_to_str(info.name);
    case 1:
      return lightusd_sv_to_str(info.prim_path);
    case 2:
      return PyUnicode_FromString(info.type <= 8 ? type_names[info.type]
                                                 : "point");
    case 3:
      return Py_BuildValue("(fff)", info.color[0], info.color[1],
                           info.color[2]);
    case 4:
      return PyFloat_FromDouble((double)info.intensity);
    case 5:
      return PyFloat_FromDouble((double)info.exposure);
    case 6:
      return matrix16f_to_python(info.transform);
    default:
      Py_RETURN_NONE;
  }
}

static PyGetSetDef RLight_getset[] = {
    {"name", RLight_get, NULL, NULL, (void*)(uintptr_t)0},
    {"prim_path", RLight_get, NULL, NULL, (void*)(uintptr_t)1},
    {"type", RLight_get, NULL, NULL, (void*)(uintptr_t)2},
    {"color", RLight_get, NULL, NULL, (void*)(uintptr_t)3},
    {"intensity", RLight_get, NULL, NULL, (void*)(uintptr_t)4},
    {"exposure", RLight_get, NULL, NULL, (void*)(uintptr_t)5},
    {"transform", RLight_get, NULL, NULL, (void*)(uintptr_t)6},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyObject* RCam_get(PyObject* self, void* closure) {
  LightUSDRenderItem* it = (LightUSDRenderItem*)self;
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;
  lightusd_render_camera_info info;
  lightusd_status s = lightusd_render_camera_get_info(item_scene(it), it->id, &info);
  if (s != LIGHTUSD_OK) return lightusd_raise(st, s, "render camera");
  switch ((uintptr_t)closure) {
    case 0:
      return lightusd_sv_to_str(info.name);
    case 1:
      return lightusd_sv_to_str(info.prim_path);
    case 2:
      return PyUnicode_FromString(info.type == 1 ? "orthographic"
                                                 : "perspective");
    case 3:
      return PyFloat_FromDouble((double)info.focal_length);
    case 4:
      return PyFloat_FromDouble((double)info.horizontal_aperture);
    case 5:
      return PyFloat_FromDouble((double)info.vertical_aperture);
    case 6:
      return PyFloat_FromDouble((double)info.near_clip);
    case 7:
      return PyFloat_FromDouble((double)info.far_clip);
    case 8:
      return PyFloat_FromDouble((double)info.fov_x);
    case 9:
      return PyFloat_FromDouble((double)info.fov_y);
    case 10:
      return matrix16f_to_python(info.transform);
    default:
      Py_RETURN_NONE;
  }
}

static PyGetSetDef RCam_getset[] = {
    {"name", RCam_get, NULL, NULL, (void*)(uintptr_t)0},
    {"prim_path", RCam_get, NULL, NULL, (void*)(uintptr_t)1},
    {"type", RCam_get, NULL, NULL, (void*)(uintptr_t)2},
    {"focal_length", RCam_get, NULL, NULL, (void*)(uintptr_t)3},
    {"horizontal_aperture", RCam_get, NULL, NULL, (void*)(uintptr_t)4},
    {"vertical_aperture", RCam_get, NULL, NULL, (void*)(uintptr_t)5},
    {"near_clip", RCam_get, NULL, NULL, (void*)(uintptr_t)6},
    {"far_clip", RCam_get, NULL, NULL, (void*)(uintptr_t)7},
    {"fov_x", RCam_get, NULL, NULL, (void*)(uintptr_t)8},
    {"fov_y", RCam_get, NULL, NULL, (void*)(uintptr_t)9},
    {"transform", RCam_get, NULL, NULL, (void*)(uintptr_t)10},
    {NULL, NULL, NULL, NULL, NULL},
};

/* ============================================================
 * RenderScene
 * ============================================================ */

static PyObject* render_item_for(lightusd_state* st, PyObject* scene_obj,
                                 PyObject* type_obj, int32_t id) {
  return item_new(st, type_obj, scene_obj, id);
}

static void RScene_dealloc(PyObject* self) {
  LightUSDRenderScene* s = (LightUSDRenderScene*)self;
  PyTypeObject* tp = Py_TYPE(self);
  if (s->scene) {
    lightusd_render_scene_destroy(s->scene);
    s->scene = NULL;
  }
  freefunc free_fn = (freefunc)PyType_GetSlot(tp, Py_tp_free);
  free_fn(self);
  Py_DECREF((PyObject*)tp);
}

/* closure encodes (render kind << 8) | type-slot selector */
static PyObject* RScene_get_items(PyObject* self, void* closure) {
  LightUSDRenderScene* s = (LightUSDRenderScene*)self;
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;
  const uintptr_t sel = (uintptr_t)closure;
  const uint8_t kind = (uint8_t)(sel & 0xFF);
  PyObject* type_obj = NULL;
  switch (kind) {
    case LIGHTUSD_RENDER_MESH:
      type_obj = st->RenderMeshType;
      break;
    case LIGHTUSD_RENDER_MATERIAL:
      type_obj = st->RenderMaterialType;
      break;
    case LIGHTUSD_RENDER_TEXTURE:
      type_obj = st->RenderTextureType;
      break;
    case LIGHTUSD_RENDER_IMAGE:
      type_obj = st->RenderImageType;
      break;
    case LIGHTUSD_RENDER_NODE:
      type_obj = st->RenderNodeType;
      break;
    case LIGHTUSD_RENDER_LIGHT:
      type_obj = st->RenderLightType;
      break;
    case LIGHTUSD_RENDER_CAMERA:
      type_obj = st->RenderCameraType;
      break;
    default:
      Py_RETURN_NONE;
  }
  size_t n = lightusd_render_count(s->scene, kind);
  PyObject* tup = PyTuple_New((Py_ssize_t)n);
  if (!tup) return NULL;
  for (size_t i = 0; i < n; ++i) {
    PyObject* item = render_item_for(st, self, type_obj, (int32_t)i);
    if (!item) {
      Py_DECREF(tup);
      return NULL;
    }
    PyTuple_SetItem(tup, (Py_ssize_t)i, item);
  }
  return tup;
}

static PyObject* RScene_get_root_nodes(PyObject* self, void* closure) {
  (void)closure;
  LightUSDRenderScene* s = (LightUSDRenderScene*)self;
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;
  size_t n = lightusd_render_count(s->scene, LIGHTUSD_RENDER_ROOT_NODE);
  PyObject* tup = PyTuple_New((Py_ssize_t)n);
  if (!tup) return NULL;
  for (size_t i = 0; i < n; ++i) {
    int32_t id = lightusd_render_root_node(s->scene, i);
    PyObject* item = render_item_for(st, self, st->RenderNodeType, id);
    if (!item) {
      Py_DECREF(tup);
      return NULL;
    }
    PyTuple_SetItem(tup, (Py_ssize_t)i, item);
  }
  return tup;
}

static PyObject* RScene_get_meta(PyObject* self, void* closure) {
  LightUSDRenderScene* s = (LightUSDRenderScene*)self;
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;
  lightusd_render_scene_info info;
  lightusd_status status = lightusd_render_scene_get_info(s->scene, &info);
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, "render scene");
  switch ((uintptr_t)closure) {
    case 0:
      return lightusd_sv_to_str(info.default_prim);
    case 1:
      return PyUnicode_FromString(info.up_axis == 1 ? "Z" : "Y");
    case 2:
      return PyFloat_FromDouble((double)info.meters_per_unit);
    case 3:
      return PyFloat_FromDouble(info.start_time);
    case 4:
      return PyFloat_FromDouble(info.end_time);
    default:
      Py_RETURN_NONE;
  }
}

static PyObject* RScene_mesh_by_path(PyObject* self, PyObject* args) {
  const char* path;
  if (!PyArg_ParseTuple(args, "s:mesh_by_path", &path)) return NULL;
  LightUSDRenderScene* s = (LightUSDRenderScene*)self;
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;
  int32_t id = lightusd_render_lookup(s->scene, LIGHTUSD_RENDER_MESH, path);
  if (id < 0) Py_RETURN_NONE;
  return render_item_for(st, self, st->RenderMeshType, id);
}

static PyObject* RScene_material_by_path(PyObject* self, PyObject* args) {
  const char* path;
  if (!PyArg_ParseTuple(args, "s:material_by_path", &path)) return NULL;
  LightUSDRenderScene* s = (LightUSDRenderScene*)self;
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;
  int32_t id = lightusd_render_lookup(s->scene, LIGHTUSD_RENDER_MATERIAL, path);
  if (id < 0) Py_RETURN_NONE;
  return render_item_for(st, self, st->RenderMaterialType, id);
}

static PyObject* RScene_warnings(PyObject* self, PyObject* noargs) {
  (void)noargs;
  LightUSDRenderScene* s = (LightUSDRenderScene*)self;
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;
  lightusd_strlist* warns = NULL;
  if (lightusd_render_scene_warnings(s->scene, &warns) != LIGHTUSD_OK) {
    return lightusd_raise(st, LIGHTUSD_ERR_INTERNAL, "warnings");
  }
  size_t n = lightusd_strlist_size(warns);
  PyObject* list = PyList_New((Py_ssize_t)n);
  if (!list) {
    lightusd_strlist_destroy(warns);
    return NULL;
  }
  for (size_t i = 0; i < n; ++i) {
    PyObject* w = lightusd_sv_to_str(lightusd_strlist_get(warns, i));
    if (!w) {
      Py_DECREF(list);
      lightusd_strlist_destroy(warns);
      return NULL;
    }
    PyList_SetItem(list, (Py_ssize_t)i, w);
  }
  lightusd_strlist_destroy(warns);
  return list;
}

static PyObject* RScene_repr(PyObject* self) {
  LightUSDRenderScene* s = (LightUSDRenderScene*)self;
  return PyUnicode_FromFormat(
      "RenderScene(meshes=%zu, materials=%zu, images=%zu)",
      lightusd_render_count(s->scene, LIGHTUSD_RENDER_MESH),
      lightusd_render_count(s->scene, LIGHTUSD_RENDER_MATERIAL),
      lightusd_render_count(s->scene, LIGHTUSD_RENDER_IMAGE));
}

/* ---- UsdVol: volumes, dense voxels, particle fields ---- */

static PyObject* float_buffer(lightusd_state* st, PyObject* scene_obj, const float* data,
                              size_t count, uint8_t components) {
  lightusd_buffer_view v;
  memset(&v, 0, sizeof(v));
  v.component_type = LIGHTUSD_COMP_FLOAT32;
  v.components = components;
  v.count = count;
  v.data = data;
  v.nbytes = count * components * sizeof(float);
  return wrap_buffer(st, scene_obj, &v);
}

static PyObject* matrix16d_to_python(const double m[16]) {
  return Py_BuildValue("((dddd)(dddd)(dddd)(dddd))", m[0], m[1], m[2], m[3], m[4], m[5], m[6],
                       m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
}

/* Set d[key] = val, stealing val. Returns -1 on error. */
static int dict_steal(PyObject* d, const char* key, PyObject* val) {
  if (!val) return -1;
  int r = PyDict_SetItemString(d, key, val);
  Py_DECREF(val);
  return r;
}

static PyObject* RScene_volumes(PyObject* self, PyObject* noargs) {
  (void)noargs;
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;
  lightusd_render_scene* scene = ((LightUSDRenderScene*)self)->scene;
  size_t n = lightusd_render_volume_count(scene);
  PyObject* out = PyList_New(0);
  if (!out) return NULL;
  for (size_t i = 0; i < n; ++i) {
    lightusd_render_volume_info vi;
    lightusd_status s = lightusd_render_volume_get_info(scene, (int32_t)i, &vi);
    if (s != LIGHTUSD_OK) { Py_DECREF(out); return lightusd_raise(st, s, "render volume"); }
    PyObject* d = PyDict_New();
    PyObject* fields = PyList_New(0);
    if (!d || !fields || PyList_Append(out, d) < 0) goto fail_vol;
    Py_DECREF(d);
    if (dict_steal(d, "prim_path", lightusd_sv_to_str(vi.prim_path)) < 0 ||
        dict_steal(d, "world_matrix", matrix16d_to_python(vi.world_matrix)) < 0 ||
        dict_steal(d, "material_id", PyLong_FromLong(vi.material_id)) < 0 ||
        dict_steal(d, "extent", vi.has_extent
                                    ? Py_BuildValue("((ddd)(ddd))", vi.extent_min[0], vi.extent_min[1],
                                                    vi.extent_min[2], vi.extent_max[0],
                                                    vi.extent_max[1], vi.extent_max[2])
                                    : (Py_INCREF(Py_None), Py_None)) < 0 ||
        dict_steal(d, "extent_is_world", PyBool_FromLong(vi.extent_is_world)) < 0 ||
        dict_steal(d, "density_scale", PyFloat_FromDouble(vi.density_scale)) < 0 ||
        dict_steal(d, "albedo", Py_BuildValue("(fff)", vi.albedo[0], vi.albedo[1], vi.albedo[2])) < 0 ||
        dict_steal(d, "emission",
                   Py_BuildValue("(fff)", vi.emission[0], vi.emission[1], vi.emission[2])) < 0 ||
        dict_steal(d, "emission_intensity", PyFloat_FromDouble(vi.emission_intensity)) < 0) {
      Py_DECREF(fields);
      Py_DECREF(out);
      return NULL;
    }
    for (uint32_t f = 0; f < vi.field_count; ++f) {
      lightusd_render_volume_field_info fi;
      s = lightusd_render_volume_field_get_info(scene, (int32_t)i, (int32_t)f, &fi);
      if (s != LIGHTUSD_OK) { Py_DECREF(fields); Py_DECREF(out); return lightusd_raise(st, s, "volume field"); }
      PyObject* fd = PyDict_New();
      if (!fd || PyList_Append(fields, fd) < 0) { Py_XDECREF(fd); Py_DECREF(fields); Py_DECREF(out); return NULL; }
      Py_DECREF(fd);
      if (dict_steal(fd, "binding", lightusd_sv_to_str(fi.binding)) < 0 ||
          dict_steal(fd, "field_prim", lightusd_sv_to_str(fi.field_prim)) < 0 ||
          dict_steal(fd, "file_path", lightusd_sv_to_str(fi.file_path)) < 0 ||
          dict_steal(fd, "grid_name", lightusd_sv_to_str(fi.grid_name)) < 0 ||
          dict_steal(fd, "grid_class", lightusd_sv_to_str(fi.grid_class)) < 0 ||
          dict_steal(fd, "value_type", lightusd_sv_to_str(fi.value_type)) < 0 ||
          dict_steal(fd, "components", PyLong_FromUnsignedLong(fi.components)) < 0 ||
          dict_steal(fd, "source_index", PyLong_FromUnsignedLong(fi.source_index)) < 0 ||
          dict_steal(fd, "index_to_world", matrix16d_to_python(fi.index_to_world)) < 0 ||
          dict_steal(fd, "world_bounds",
                     fi.has_bounds ? Py_BuildValue("((ddd)(ddd))", fi.world_min[0], fi.world_min[1],
                                                   fi.world_min[2], fi.world_max[0], fi.world_max[1],
                                                   fi.world_max[2])
                                   : (Py_INCREF(Py_None), Py_None)) < 0) {
        Py_DECREF(fields);
        Py_DECREF(out);
        return NULL;
      }
    }
    if (dict_steal(d, "fields", fields) < 0) { Py_DECREF(out); return NULL; }
    continue;
  fail_vol:
    Py_XDECREF(d);
    Py_XDECREF(fields);
    Py_DECREF(out);
    return NULL;
  }
  return out;
}

static PyObject* RScene_volume_dense(PyObject* self, PyObject* args, PyObject* kw) {
  static char* kwlist[] = {"volume", "field", "max_voxels", "downsample", "vector", NULL};
  int volume = 0, field = 0, downsample = 0, vector = 0;
  unsigned long long max_voxels = 0;
  if (!PyArg_ParseTupleAndKeywords(args, kw, "ii|Kpp", kwlist, &volume, &field, &max_voxels,
                                   &downsample, &vector))
    return NULL;
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;
  lightusd_render_volume_dense dv;
  lightusd_status s = lightusd_render_volume_field_dense(((LightUSDRenderScene*)self)->scene, volume,
                                                         field, max_voxels, downsample, vector, &dv);
  if (s != LIGHTUSD_OK) return lightusd_raise(st, s, "volume dense");
  const size_t cells = (size_t)dv.dims[0] * (size_t)dv.dims[1] * (size_t)dv.dims[2];
  PyObject* d = PyDict_New();
  if (!d) return NULL;
  if (dict_steal(d, "data", float_buffer(st, self, dv.data, cells, (uint8_t)dv.components)) < 0 ||
      dict_steal(d, "dims", Py_BuildValue("(iii)", dv.dims[0], dv.dims[1], dv.dims[2])) < 0 ||
      dict_steal(d, "index_origin", Py_BuildValue("(iii)", dv.index_origin[0], dv.index_origin[1],
                                                  dv.index_origin[2])) < 0 ||
      dict_steal(d, "stride", PyLong_FromLong(dv.stride)) < 0 ||
      dict_steal(d, "components", PyLong_FromUnsignedLong(dv.components)) < 0 ||
      dict_steal(d, "background", PyFloat_FromDouble(dv.background)) < 0 ||
      dict_steal(d, "cell_to_world", matrix16d_to_python(dv.cell_to_world)) < 0) {
    Py_DECREF(d);
    return NULL;
  }
  return d;
}

static PyObject* RScene_particle_fields(PyObject* self, PyObject* noargs) {
  (void)noargs;
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;
  lightusd_render_scene* scene = ((LightUSDRenderScene*)self)->scene;
  size_t n = lightusd_render_particle_data_count(scene);
  PyObject* out = PyList_New(0);
  if (!out) return NULL;
  for (size_t i = 0; i < n; ++i) {
    lightusd_render_particle_data p;
    lightusd_status s = lightusd_render_particle_data_get(scene, (int32_t)i, &p);
    if (s != LIGHTUSD_OK) { Py_DECREF(out); return lightusd_raise(st, s, "particle field"); }
    const size_t c = (size_t)p.count;
    const uint8_t sh = (uint8_t)((p.sh_degree + 1) * (p.sh_degree + 1) * 3);
    PyObject* d = PyDict_New();
    if (!d || PyList_Append(out, d) < 0) { Py_XDECREF(d); Py_DECREF(out); return NULL; }
    Py_DECREF(d);
    if (dict_steal(d, "prim_path", lightusd_sv_to_str(p.prim_path)) < 0 ||
        dict_steal(d, "type_name", lightusd_sv_to_str(p.type_name)) < 0 ||
        dict_steal(d, "kernel", lightusd_sv_to_str(p.kernel)) < 0 ||
        dict_steal(d, "projection_mode_hint", lightusd_sv_to_str(p.projection_mode_hint)) < 0 ||
        dict_steal(d, "sorting_mode_hint", lightusd_sv_to_str(p.sorting_mode_hint)) < 0 ||
        dict_steal(d, "count", PyLong_FromUnsignedLongLong(p.count)) < 0 ||
        dict_steal(d, "sh_degree", PyLong_FromLong(p.sh_degree)) < 0 ||
        dict_steal(d, "world_matrix", matrix16d_to_python(p.world_matrix)) < 0 ||
        dict_steal(d, "positions", float_buffer(st, self, p.positions, c, 3)) < 0 ||
        dict_steal(d, "orientations", float_buffer(st, self, p.orientations, c, 4)) < 0 ||
        dict_steal(d, "scales", float_buffer(st, self, p.scales, c, 3)) < 0 ||
        dict_steal(d, "opacities", float_buffer(st, self, p.opacities, c, 1)) < 0 ||
        dict_steal(d, "sh_coefficients", float_buffer(st, self, p.sh_coefficients, c, sh)) < 0) {
      Py_DECREF(out);
      return NULL;
    }
  }
  return out;
}

static PyMethodDef RScene_methods[] = {
    {"volumes", RScene_volumes, METH_NOARGS,
     "UsdVol Volumes: list of dicts (prim_path, world_matrix, extent, material, fields)."},
    {"volume_dense", (PyCFunction)(void (*)(void))RScene_volume_dense,
     METH_VARARGS | METH_KEYWORDS,
     "volume_dense(volume, field, max_voxels=0, downsample=False, vector=False) -> dict "
     "with float voxels (x fastest), dims, cell_to_world."},
    {"particle_fields", RScene_particle_fields, METH_NOARGS,
     "UsdVol ParticleFields (3DGS): list of dicts with per-particle arrays."},
    {"mesh_by_path", RScene_mesh_by_path, METH_VARARGS,
     "mesh_by_path(prim_path) -> RenderMesh | None"},
    {"material_by_path", RScene_material_by_path, METH_VARARGS,
     "material_by_path(prim_path) -> RenderMaterial | None"},
    {"warnings", RScene_warnings, METH_NOARGS,
     "Conversion warnings (list of str)."},
    {NULL, NULL, 0, NULL},
};

PyObject* lightusd_physics_annotations(lightusd_state*, lightusd_render_scene*);
static PyObject* RScene_get_physics(PyObject* self, void* closure) {
  (void)closure;
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;
  return lightusd_physics_annotations(st, ((LightUSDRenderScene*)self)->scene);
}

static PyGetSetDef RScene_getset[] = {
    {"physics", RScene_get_physics, NULL, "Retained USD Physics annotations.",
     NULL},
    {"meshes", RScene_get_items, NULL, NULL,
     (void*)(uintptr_t)LIGHTUSD_RENDER_MESH},
    {"materials", RScene_get_items, NULL, NULL,
     (void*)(uintptr_t)LIGHTUSD_RENDER_MATERIAL},
    {"textures", RScene_get_items, NULL, NULL,
     (void*)(uintptr_t)LIGHTUSD_RENDER_TEXTURE},
    {"images", RScene_get_items, NULL, NULL,
     (void*)(uintptr_t)LIGHTUSD_RENDER_IMAGE},
    {"nodes", RScene_get_items, NULL, NULL,
     (void*)(uintptr_t)LIGHTUSD_RENDER_NODE},
    {"lights", RScene_get_items, NULL, NULL,
     (void*)(uintptr_t)LIGHTUSD_RENDER_LIGHT},
    {"cameras", RScene_get_items, NULL, NULL,
     (void*)(uintptr_t)LIGHTUSD_RENDER_CAMERA},
    {"root_nodes", RScene_get_root_nodes, NULL, NULL, NULL},
    {"default_prim", RScene_get_meta, NULL, NULL, (void*)(uintptr_t)0},
    {"up_axis", RScene_get_meta, NULL, NULL, (void*)(uintptr_t)1},
    {"meters_per_unit", RScene_get_meta, NULL, NULL, (void*)(uintptr_t)2},
    {"start_time", RScene_get_meta, NULL, NULL, (void*)(uintptr_t)3},
    {"end_time", RScene_get_meta, NULL, NULL, (void*)(uintptr_t)4},
    {NULL, NULL, NULL, NULL, NULL},
};

/* ============================================================
 * Type specs + module function
 * ============================================================ */

#ifdef Py_TPFLAGS_IMMUTABLETYPE
#define LIGHTUSD_ITEM_FLAGS (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE)
#else
#define LIGHTUSD_ITEM_FLAGS Py_TPFLAGS_DEFAULT
#endif

#define LIGHTUSD_ITEM_SPEC(pyname, methods_, getset_, repr_)          \
  static PyType_Slot pyname##_slots[] = {                         \
      {Py_tp_dealloc, (void*)item_dealloc},                       \
      {Py_tp_repr, (void*)repr_},                                 \
      {Py_tp_methods, (void*)methods_},                           \
      {Py_tp_getset, (void*)getset_},                             \
      {0, NULL},                                                  \
  };                                                              \
  static PyType_Spec pyname##_spec = {                            \
      .name = "lightusd._core." #pyname,                          \
      .basicsize = sizeof(LightUSDRenderItem),                        \
      .flags = LIGHTUSD_ITEM_FLAGS,                                   \
      .slots = pyname##_slots,                                    \
  }

static PyMethodDef no_methods[] = {{NULL, NULL, 0, NULL}};

static PyObject* generic_item_repr(PyObject* self) {
  return PyUnicode_FromFormat("%s(id=%d)", "RenderItem",
                              ((LightUSDRenderItem*)self)->id);
}

LIGHTUSD_ITEM_SPEC(RenderMesh, RMesh_methods, RMesh_getset, RMesh_repr);
LIGHTUSD_ITEM_SPEC(RenderMaterial, RMat_methods, RMat_getset, RMat_repr);
LIGHTUSD_ITEM_SPEC(RenderTexture, no_methods, RTex_getset, RTex_repr);
LIGHTUSD_ITEM_SPEC(RenderImage, no_methods, RImg_getset, RImg_repr);
LIGHTUSD_ITEM_SPEC(RenderNode, no_methods, RNode_getset, RNode_repr);
LIGHTUSD_ITEM_SPEC(RenderLight, no_methods, RLight_getset, generic_item_repr);
LIGHTUSD_ITEM_SPEC(RenderCamera, no_methods, RCam_getset, generic_item_repr);

static PyType_Slot RScene_slots[] = {
    {Py_tp_dealloc, (void*)RScene_dealloc},
    {Py_tp_repr, (void*)RScene_repr},
    {Py_tp_methods, (void*)RScene_methods},
    {Py_tp_getset, (void*)RScene_getset},
    {0, NULL},
};

static PyType_Spec RScene_spec = {
    .name = "lightusd._core.RenderScene",
    .basicsize = sizeof(LightUSDRenderScene),
    .flags = LIGHTUSD_ITEM_FLAGS,
    .slots = RScene_slots,
};

PyObject* lightusd_to_render_scene(PyObject* module, PyObject* args,
                               PyObject* kwargs) {
  static char* kwlist[] = {"stage",
                           "triangulate",
                           "compute_normals",
                           "compute_tangents",
                           "load_textures",
                           "time",
                           "duplicate_instance_meshes",
                           NULL};
  PyObject* stage_obj;
  int triangulate = 1;
  int compute_normals = 1;
  int compute_tangents = 0;
  int load_textures = 1;
  double time = 0.0;
  int duplicate = 0;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|ppppdp:to_render_scene",
                                   kwlist, &stage_obj, &triangulate,
                                   &compute_normals, &compute_tangents,
                                   &load_textures, &time, &duplicate)) {
    return NULL;
  }
  lightusd_state* st = lightusd_state_from_module(module);
  if (!st) return NULL;
  if (!PyObject_TypeCheck(stage_obj, (PyTypeObject*)st->StageType)) {
    PyErr_SetString(PyExc_TypeError, "expected a lightusd.Stage");
    return NULL;
  }
  lightusd_stage* stage = lightusd_stage_handle(stage_obj);
  if (!stage) {
    PyErr_SetString(st->UsdError, "Stage is closed");
    return NULL;
  }

  lightusd_render_config cfg;
  lightusd_render_config_init(&cfg);
  cfg.triangulate = triangulate ? 1 : 0;
  cfg.compute_normals = compute_normals ? 1 : 0;
  cfg.compute_tangents = compute_tangents ? 1 : 0;
  cfg.load_textures = load_textures ? 1 : 0;
  cfg.duplicate_instance_meshes = duplicate ? 1 : 0;
  cfg.time_code = time;

  lightusd_render_scene* scene = NULL;
  lightusd_status status;
  Py_BEGIN_ALLOW_THREADS
  status = lightusd_render_convert(stage, &cfg, &scene);
  Py_END_ALLOW_THREADS
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, "to_render_scene");

  PyObject* obj = lightusd_alloc(st->RenderSceneType);
  if (!obj) {
    lightusd_render_scene_destroy(scene);
    return NULL;
  }
  ((LightUSDRenderScene*)obj)->scene = scene;
  return obj;
}

int lightusd_register_render_types(PyObject* module, lightusd_state* st) {
#define REG(field, spec, exported)                                     \
  do {                                                                 \
    st->field = PyType_FromModuleAndSpec(module, &spec, NULL);         \
    if (!st->field) return -1;                                         \
    if (PyModule_AddObjectRef(module, exported, st->field) < 0) {      \
      return -1;                                                       \
    }                                                                  \
  } while (0)

  REG(RenderSceneType, RScene_spec, "RenderScene");
  REG(RenderMeshType, RenderMesh_spec, "RenderMesh");
  REG(RenderMaterialType, RenderMaterial_spec, "RenderMaterial");
  REG(RenderTextureType, RenderTexture_spec, "RenderTexture");
  REG(RenderImageType, RenderImage_spec, "RenderImage");
  REG(RenderNodeType, RenderNode_spec, "RenderNode");
  REG(RenderLightType, RenderLight_spec, "RenderLight");
  REG(RenderCameraType, RenderCamera_spec, "RenderCamera");
#undef REG
  return 0;
}

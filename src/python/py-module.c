/* SPDX-License-Identifier: Apache-2.0
 * lightusd._core — module definition, state, exceptions, loaders.
 */

#include "py-internal.h"

/* ============================================================
 * Generic helpers
 * ============================================================ */

lightusd_state* lightusd_state_from_type(PyTypeObject* tp) {
  PyObject* module = PyType_GetModule(tp);
  if (!module) return NULL;
  return (lightusd_state*)PyModule_GetState(module);
}

PyObject* lightusd_alloc(PyObject* type_obj) {
  PyTypeObject* tp = (PyTypeObject*)type_obj;
  allocfunc alloc = (allocfunc)PyType_GetSlot(tp, Py_tp_alloc);
  if (!alloc) {
    PyErr_SetString(PyExc_SystemError, "type has no allocator");
    return NULL;
  }
  return alloc(tp, 0);
}

PyObject* lightusd_raise(lightusd_state* st, lightusd_status status, const char* what) {
  const char* detail = lightusd_last_error();
  if (!detail || !detail[0]) detail = "(no detail)";
  PyObject* exc;
  switch (status) {
    case LIGHTUSD_ERR_IO:
      exc = st->UsdIoError;
      break;
    case LIGHTUSD_ERR_PARSE:
      exc = st->UsdParseError;
      break;
    case LIGHTUSD_ERR_NOT_FOUND:
      exc = PyExc_KeyError;
      break;
    case LIGHTUSD_ERR_TYPE_MISMATCH:
      exc = PyExc_TypeError;
      break;
    case LIGHTUSD_ERR_INVALID_ARG:
      exc = PyExc_ValueError;
      break;
    case LIGHTUSD_ERR_OUT_OF_MEMORY:
      exc = PyExc_MemoryError;
      break;
    default:
      exc = st->UsdError;
      break;
  }
  if (what && what[0]) {
    PyErr_Format(exc, "%s: %s", what, detail);
  } else {
    PyErr_Format(exc, "%s", detail);
  }
  return NULL;
}

PyObject* lightusd_wrap_stage(lightusd_state* st, lightusd_stage* stage) {
  PyObject* obj = lightusd_alloc(st->StageType);
  if (!obj) {
    lightusd_stage_destroy(stage);
    return NULL;
  }
  ((LightUSDStage*)obj)->stage = stage;
  return obj;
}

/* ============================================================
 * ValueBlock sentinel
 * ============================================================ */

static PyObject* ValueBlock_repr(PyObject* self) {
  (void)self;
  return PyUnicode_FromString("ValueBlock");
}

static PyType_Slot ValueBlock_slots[] = {
    {Py_tp_repr, (void*)ValueBlock_repr},
    {0, NULL},
};

static PyType_Spec ValueBlock_spec = {
    .name = "lightusd._core._ValueBlockType",
    .basicsize = sizeof(PyObject),
    .flags = Py_TPFLAGS_DEFAULT
#ifdef Py_TPFLAGS_IMMUTABLETYPE
             | Py_TPFLAGS_IMMUTABLETYPE
#endif
    ,
    .slots = ValueBlock_slots,
};

/* ============================================================
 * Loaders
 * ============================================================ */

typedef struct {
  const char** sets;
  const char** names;
  PyObject** tmps; /* 2*n bytes objects to release */
  size_t count;
} variant_overrides;

static void variants_clear(variant_overrides* v) {
  for (size_t i = 0; i < v->count * 2; ++i) Py_XDECREF(v->tmps[i]);
  PyMem_Free(v->sets);
  PyMem_Free(v->names);
  PyMem_Free(v->tmps);
  memset(v, 0, sizeof(*v));
}

static int variants_from_dict(PyObject* dict, variant_overrides* v) {
  memset(v, 0, sizeof(*v));
  if (!dict || dict == Py_None) return 0;
  if (!PyDict_Check(dict)) {
    PyErr_SetString(PyExc_TypeError,
                    "variants must be a dict of {set_name: variant_name}");
    return -1;
  }
  Py_ssize_t n = PyDict_Size(dict);
  if (n <= 0) return 0;
  size_t alloc_n = (size_t)n;
  if (alloc_n > SIZE_MAX / sizeof(char*) ||
      alloc_n > SIZE_MAX / sizeof(PyObject*) / 2) {
    PyErr_NoMemory();
    return -1;
  }
  v->sets = (const char**)PyMem_Malloc(alloc_n * sizeof(char*));
  v->names = (const char**)PyMem_Malloc(alloc_n * sizeof(char*));
  v->tmps = (PyObject**)PyMem_Malloc(alloc_n * 2 * sizeof(PyObject*));
  if (!v->sets || !v->names || !v->tmps) {
    PyMem_Free(v->sets);
    PyMem_Free(v->names);
    PyMem_Free(v->tmps);
    memset(v, 0, sizeof(*v));
    PyErr_NoMemory();
    return -1;
  }
  PyObject *key, *value;
  Py_ssize_t pos = 0;
  while (PyDict_Next(dict, &pos, &key, &value)) {
    size_t i = v->count;
    v->sets[i] = lightusd_utf8(key, &v->tmps[i * 2]);
    v->names[i] = lightusd_utf8(value, &v->tmps[i * 2 + 1]);
    if (!v->sets[i] || !v->names[i]) {
      v->count = i + 1;
      variants_clear(v);
      return -1;
    }
    v->count = i + 1;
  }
  return 0;
}

static int parse_format(const char* format, uint32_t* out) {
  if (!format) {
    *out = LIGHTUSD_FORMAT_AUTO;
    return 0;
  }
  if (strcmp(format, "usda") == 0) {
    *out = LIGHTUSD_FORMAT_USDA;
  } else if (strcmp(format, "usdc") == 0) {
    *out = LIGHTUSD_FORMAT_USDC;
  } else if (strcmp(format, "usdz") == 0) {
    *out = LIGHTUSD_FORMAT_USDZ;
  } else {
    PyErr_SetString(PyExc_ValueError,
                    "format must be 'usda', 'usdc' or 'usdz'");
    return -1;
  }
  return 0;
}

static PyObject* mod_load(PyObject* module, PyObject* args, PyObject* kwargs) {
  static char* kwlist[] = {"path",     "format",       "composed",
                           "variants", "load_payloads", "max_memory", NULL};
  const char* path;
  const char* format = NULL;
  int composed = 1;
  PyObject* variants = Py_None;
  int load_payloads = 1;
  unsigned long long max_memory = 0;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|zpOpK:load", kwlist,
                                   &path, &format, &composed, &variants,
                                   &load_payloads, &max_memory)) {
    return NULL;
  }
  lightusd_state* st = lightusd_state_from_module(module);
  if (!st) return NULL;

  lightusd_load_options opts;
  lightusd_load_options_init(&opts);
  if (parse_format(format, &opts.format) != 0) return NULL;
  opts.composed = composed ? 1 : 0;
  opts.load_payloads = load_payloads ? 1 : 0;
  if (max_memory) {
    opts.max_input_bytes = max_memory;
    opts.max_asset_bytes = max_memory;
    opts.max_resident_bytes = max_memory;
  }

  variant_overrides vo;
  if (variants_from_dict(variants, &vo) != 0) return NULL;
  opts.variant_sets = vo.sets;
  opts.variant_names = vo.names;
  opts.variant_override_count = vo.count;

  lightusd_stage* stage = NULL;
  lightusd_status status;
  Py_BEGIN_ALLOW_THREADS
  status = lightusd_stage_load(path, &opts, &stage);
  Py_END_ALLOW_THREADS
  variants_clear(&vo);
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, path);
  return lightusd_wrap_stage(st, stage);
}

static PyObject* load_from_buffer(lightusd_state* st, const uint8_t* data,
                                  size_t size, const char* format,
                                  unsigned long long max_memory) {
  lightusd_load_options opts;
  lightusd_load_options_init(&opts);
  if (parse_format(format, &opts.format) != 0) return NULL;
  if (max_memory) {
    opts.max_input_bytes = max_memory;
    opts.max_asset_bytes = max_memory;
    opts.max_resident_bytes = max_memory;
  }

  lightusd_stage* stage = NULL;
  lightusd_status status;
  Py_BEGIN_ALLOW_THREADS
  status = lightusd_stage_load_from_memory(data, size, &opts, &stage);
  Py_END_ALLOW_THREADS
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, "load from memory");
  return lightusd_wrap_stage(st, stage);
}

static PyObject* mod_loads(PyObject* module, PyObject* args) {
  PyObject* text;
  if (!PyArg_ParseTuple(args, "U:loads", &text)) return NULL;
  lightusd_state* st = lightusd_state_from_module(module);
  if (!st) return NULL;
  PyObject* tmp = NULL;
  const char* data = lightusd_utf8(text, &tmp);
  if (!data) return NULL;
  Py_ssize_t size = PyBytes_Size(tmp);
  PyObject* res = load_from_buffer(st, (const uint8_t*)data, (size_t)size,
                                   "usda", 0);
  Py_XDECREF(tmp);
  return res;
}

static PyObject* mod_load_bytes(PyObject* module, PyObject* args,
                                PyObject* kwargs) {
  static char* kwlist[] = {"data", "format", "max_memory", NULL};
  PyObject* data_obj;
  const char* format = NULL;
  unsigned long long max_memory = 0;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|zK:load_bytes", kwlist,
                                   &data_obj, &format, &max_memory)) {
    return NULL;
  }
  lightusd_state* st = lightusd_state_from_module(module);
  if (!st) return NULL;

  /* Accept bytes / bytearray / any buffer via bytes(); the C loader copies
   * into its own structures anyway. */
  PyObject* bytes = PyBytes_FromObject(data_obj);
  if (!bytes) return NULL;
  char* buf = NULL;
  Py_ssize_t size = 0;
  if (PyBytes_AsStringAndSize(bytes, &buf, &size) != 0) {
    Py_DECREF(bytes);
    return NULL;
  }
  PyObject* res = load_from_buffer(st, (const uint8_t*)buf, (size_t)size,
                                   format, max_memory);
  Py_DECREF(bytes);
  return res;
}

static PyObject* mod_flatten_file(PyObject* module, PyObject* args,
                                  PyObject* kwargs) {
  static char* kwlist[] = {"src", "dst", "max_memory", NULL};
  const char* src;
  const char* dst;
  unsigned long long max_memory = 0;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "ss|K:flatten_file", kwlist,
                                   &src, &dst, &max_memory)) {
    return NULL;
  }
  lightusd_state* st = lightusd_state_from_module(module);
  if (!st) return NULL;
  lightusd_load_options opts;
  lightusd_load_options_init(&opts);
  if (max_memory) {
    opts.max_input_bytes = max_memory;
    opts.max_asset_bytes = max_memory;
    opts.max_resident_bytes = max_memory;
  }
  lightusd_status status;
  Py_BEGIN_ALLOW_THREADS
  status = lightusd_flatten_file_to_usdc(src, dst, &opts);
  Py_END_ALLOW_THREADS
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, src);
  Py_RETURN_NONE;
}

static PyObject* mod_set_normalizer(PyObject* module, PyObject* args) {
  PyObject* fn;
  if (!PyArg_ParseTuple(args, "O:_set_normalizer", &fn)) return NULL;
  lightusd_state* st = lightusd_state_from_module(module);
  if (!st) return NULL;
  PyObject* old = st->normalizer;
  if (fn == Py_None) {
    st->normalizer = NULL;
  } else {
    Py_INCREF(fn);
    st->normalizer = fn;
  }
  Py_XDECREF(old);
  Py_RETURN_NONE;
}

static PyObject* mod_type_from_name(PyObject* module, PyObject* args) {
  (void)module;
  const char* name;
  if (!PyArg_ParseTuple(args, "s:type_from_name", &name)) return NULL;
  return PyLong_FromLong((long)lightusd_type_from_name(name));
}

static PyObject* mod_type_name(PyObject* module, PyObject* args) {
  (void)module;
  int type;
  if (!PyArg_ParseTuple(args, "i:type_name", &type)) return NULL;
  return PyUnicode_FromString(lightusd_type_name((lightusd_type)type));
}

static PyMethodDef module_methods[] = {
    {"load", (PyCFunction)(void (*)(void))mod_load,
     METH_VARARGS | METH_KEYWORDS,
     "load(path, *, format=None, composed=True, variants=None, "
     "load_payloads=True, max_memory=0) -> Stage"},
    {"loads", mod_loads, METH_VARARGS, "loads(usda_text) -> Stage"},
    {"load_bytes", (PyCFunction)(void (*)(void))mod_load_bytes,
     METH_VARARGS | METH_KEYWORDS,
     "load_bytes(data, *, format=None, max_memory=0) -> Stage"},
    {"flatten_file", (PyCFunction)(void (*)(void))mod_flatten_file,
     METH_VARARGS | METH_KEYWORDS,
     "flatten_file(src, dst, *, max_memory=0)\n"
     "Low-memory flatten of a USD file into a USDC file."},
    {"to_render_scene", (PyCFunction)(void (*)(void))lightusd_to_render_scene,
     METH_VARARGS | METH_KEYWORDS,
     "to_render_scene(stage, *, triangulate=True, compute_normals=True, "
     "compute_tangents=False, load_textures=True, time=0.0, "
     "duplicate_instance_meshes=False) -> RenderScene"},
    {"_set_normalizer", mod_set_normalizer, METH_VARARGS,
     "Register the value normalizer used by authoring APIs (internal)."},
    {"type_from_name", mod_type_from_name, METH_VARARGS,
     "type_from_name(usd_type_name) -> int"},
    {"type_name", mod_type_name, METH_VARARGS,
     "type_name(type_id) -> str"},
    {NULL, NULL, 0, NULL},
};

/* ============================================================
 * Module lifecycle
 * ============================================================ */

static int module_exec(PyObject* module) {
  lightusd_state* st = lightusd_state_from_module(module);
  if (!st) return -1;

  st->UsdError =
      PyErr_NewExceptionWithDoc("lightusd._core.UsdError",
                                "Base error for lightusd.", NULL, NULL);
  if (!st->UsdError) return -1;
  if (PyModule_AddObjectRef(module, "UsdError", st->UsdError) < 0) return -1;

  {
    PyObject* bases = PyTuple_Pack(2, st->UsdError, PyExc_ValueError);
    if (!bases) return -1;
    st->UsdParseError = PyErr_NewExceptionWithDoc(
        "lightusd._core.UsdParseError", "USD parsing failed.", bases, NULL);
    Py_DECREF(bases);
    if (!st->UsdParseError) return -1;
    if (PyModule_AddObjectRef(module, "UsdParseError", st->UsdParseError) <
        0) {
      return -1;
    }
  }
  {
    PyObject* bases = PyTuple_Pack(2, st->UsdError, PyExc_OSError);
    if (!bases) return -1;
    st->UsdIoError = PyErr_NewExceptionWithDoc(
        "lightusd._core.UsdIoError", "USD file I/O failed.", bases, NULL);
    Py_DECREF(bases);
    if (!st->UsdIoError) return -1;
    if (PyModule_AddObjectRef(module, "UsdIoError", st->UsdIoError) < 0) {
      return -1;
    }
  }
  st->StaleHandleError = PyErr_NewExceptionWithDoc(
      "lightusd._core.StaleHandleError",
      "A Prim handle outlived a structural stage edit.", st->UsdError, NULL);
  if (!st->StaleHandleError) return -1;
  if (PyModule_AddObjectRef(module, "StaleHandleError",
                            st->StaleHandleError) < 0) {
    return -1;
  }

  if (lightusd_register_array_type(module, st) < 0) return -1;
  if (lightusd_register_prim_types(module, st) < 0) return -1;
  if (lightusd_register_stage_type(module, st) < 0) return -1;
  if (lightusd_register_render_types(module, st) < 0) return -1;

  /* ValueBlock sentinel */
  {
    PyObject* vb_type =
        PyType_FromModuleAndSpec(module, &ValueBlock_spec, NULL);
    if (!vb_type) return -1;
    st->ValueBlock = lightusd_alloc(vb_type);
    Py_DECREF(vb_type);
    if (!st->ValueBlock) return -1;
    if (PyModule_AddObjectRef(module, "ValueBlock", st->ValueBlock) < 0) {
      return -1;
    }
  }

  if (PyModule_AddStringConstant(module, "__version__",
                                 lightusd_version_string()) < 0) {
    return -1;
  }
  return 0;
}

static int module_traverse(PyObject* module, visitproc visit, void* arg) {
  lightusd_state* st = lightusd_state_from_module(module);
  if (!st) return 0;
  Py_VISIT(st->UsdError);
  Py_VISIT(st->UsdParseError);
  Py_VISIT(st->UsdIoError);
  Py_VISIT(st->StaleHandleError);
  Py_VISIT(st->StageType);
  Py_VISIT(st->StageIterType);
  Py_VISIT(st->PrimType);
  Py_VISIT(st->PrimChildIterType);
  Py_VISIT(st->AttributeType);
  Py_VISIT(st->RelationshipType);
  Py_VISIT(st->TimeSamplesType);
  Py_VISIT(st->VariantSetsType);
  Py_VISIT(st->VariantSetType);
  Py_VISIT(st->ArrayType);
  Py_VISIT(st->RenderSceneType);
  Py_VISIT(st->RenderMeshType);
  Py_VISIT(st->RenderMaterialType);
  Py_VISIT(st->RenderTextureType);
  Py_VISIT(st->RenderImageType);
  Py_VISIT(st->RenderNodeType);
  Py_VISIT(st->RenderLightType);
  Py_VISIT(st->RenderCameraType);
  Py_VISIT(st->ValueBlock);
  Py_VISIT(st->normalizer);
  return 0;
}

static int module_clear(PyObject* module) {
  lightusd_state* st = lightusd_state_from_module(module);
  if (!st) return 0;
  Py_CLEAR(st->UsdError);
  Py_CLEAR(st->UsdParseError);
  Py_CLEAR(st->UsdIoError);
  Py_CLEAR(st->StaleHandleError);
  Py_CLEAR(st->StageType);
  Py_CLEAR(st->StageIterType);
  Py_CLEAR(st->PrimType);
  Py_CLEAR(st->PrimChildIterType);
  Py_CLEAR(st->AttributeType);
  Py_CLEAR(st->RelationshipType);
  Py_CLEAR(st->TimeSamplesType);
  Py_CLEAR(st->VariantSetsType);
  Py_CLEAR(st->VariantSetType);
  Py_CLEAR(st->ArrayType);
  Py_CLEAR(st->RenderSceneType);
  Py_CLEAR(st->RenderMeshType);
  Py_CLEAR(st->RenderMaterialType);
  Py_CLEAR(st->RenderTextureType);
  Py_CLEAR(st->RenderImageType);
  Py_CLEAR(st->RenderNodeType);
  Py_CLEAR(st->RenderLightType);
  Py_CLEAR(st->RenderCameraType);
  Py_CLEAR(st->ValueBlock);
  Py_CLEAR(st->normalizer);
  return 0;
}

static void module_free(void* module) { module_clear((PyObject*)module); }

static PyModuleDef_Slot module_slots[] = {
    {Py_mod_exec, (void*)module_exec},
#ifdef Py_mod_multiple_interpreters
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
#endif
#ifdef Py_GIL_DISABLED
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
#endif
    {0, NULL},
};

static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    .m_name = "lightusd._core",
    .m_doc = "LightUSD core binding (next-core + tydra-next).",
    .m_size = sizeof(lightusd_state),
    .m_methods = module_methods,
    .m_slots = module_slots,
    .m_traverse = module_traverse,
    .m_clear = module_clear,
    .m_free = module_free,
};

PyMODINIT_FUNC PyInit__core(void) { return PyModuleDef_Init(&moduledef); }

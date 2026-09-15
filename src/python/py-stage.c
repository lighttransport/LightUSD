/* SPDX-License-Identifier: Apache-2.0
 * lightusd._core — Stage type and depth-first stage iterator.
 */

#include "py-internal.h"

/* Defined in py-module.c */
PyObject* lightusd_wrap_stage(lightusd_state* st, lightusd_stage* stage);

/* ============================================================
 * Stage DFS iterator
 * ============================================================ */

static void StageIter_dealloc(PyObject* self) {
  LightUSDStageIter* it = (LightUSDStageIter*)self;
  PyTypeObject* tp = Py_TYPE(self);
  Py_CLEAR(it->stage);
  PyMem_Free(it->stack);
  it->stack = NULL;
  freefunc free_fn = (freefunc)PyType_GetSlot(tp, Py_tp_free);
  free_fn(self);
  Py_DECREF((PyObject*)tp);
}

static int stageiter_push(LightUSDStageIter* it, lightusd_prim prim) {
  if (it->top == it->cap) {
    size_t newcap = it->cap ? (size_t)it->cap * 2 : 64;
    if (newcap > SIZE_MAX / sizeof(lightusd_prim)) {
      PyErr_NoMemory();
      return -1;
    }
    lightusd_prim* mem = (lightusd_prim*)PyMem_Realloc(
        it->stack, newcap * sizeof(lightusd_prim));
    if (!mem) {
      PyErr_NoMemory();
      return -1;
    }
    it->cap = (Py_ssize_t)newcap;
    it->stack = mem;
    it->cap = newcap;
  }
  it->stack[it->top++] = prim;
  return 0;
}

static PyObject* StageIter_next(PyObject* self) {
  LightUSDStageIter* it = (LightUSDStageIter*)self;
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;
  lightusd_stage* stage = lightusd_stage_handle(it->stage);
  if (!stage) {
    PyErr_SetString(st->UsdError, "Stage is closed");
    return NULL;
  }
  if (it->gen != lightusd_stage_generation(stage)) {
    PyErr_SetString(st->StaleHandleError,
                    "Stage was structurally modified during traversal");
    return NULL;
  }
  if (it->top == 0) return NULL; /* StopIteration */
  lightusd_prim prim = it->stack[--it->top];
  /* push children in reverse so traversal is preorder, left-to-right */
  size_t n = lightusd_prim_child_count(prim);
  for (size_t i = n; i > 0; --i) {
    lightusd_prim child = lightusd_prim_child(prim, i - 1);
    if (lightusd_prim_is_valid(child)) {
      if (stageiter_push(it, child) != 0) return NULL;
    }
  }
  return lightusd_wrap_prim(st, it->stage, prim);
}

static PyObject* StageIter_iter(PyObject* self) {
  Py_INCREF(self);
  return self;
}

static PyType_Slot StageIter_slots[] = {
    {Py_tp_dealloc, (void*)StageIter_dealloc},
    {Py_tp_iter, (void*)StageIter_iter},
    {Py_tp_iternext, (void*)StageIter_next},
    {0, NULL},
};

static PyType_Spec StageIter_spec = {
    .name = "lightusd._core._StageIterator",
    .basicsize = sizeof(LightUSDStageIter),
    .flags = Py_TPFLAGS_DEFAULT
#ifdef Py_TPFLAGS_IMMUTABLETYPE
             | Py_TPFLAGS_IMMUTABLETYPE
#endif
    ,
    .slots = StageIter_slots,
};

/* Create a DFS iterator over the stage (roots) or a prim's subtree. */
static PyObject* stage_iter_new(lightusd_state* st, PyObject* stage_obj) {
  lightusd_stage* stage = lightusd_stage_handle(stage_obj);
  if (!stage) {
    PyErr_SetString(st->UsdError, "Stage is closed");
    return NULL;
  }
  PyObject* obj = lightusd_alloc(st->StageIterType);
  if (!obj) return NULL;
  LightUSDStageIter* it = (LightUSDStageIter*)obj;
  it->stage = stage_obj;
  Py_INCREF(stage_obj);
  it->stack = NULL;
  it->top = 0;
  it->cap = 0;
  it->gen = lightusd_stage_generation(stage);
  size_t n = lightusd_stage_root_prim_count(stage);
  for (size_t i = n; i > 0; --i) {
    lightusd_prim root = lightusd_stage_root_prim(stage, i - 1);
    if (lightusd_prim_is_valid(root)) {
      if (stageiter_push(it, root) != 0) {
        Py_DECREF(obj);
        return NULL;
      }
    }
  }
  return obj;
}

/* ============================================================
 * Stage
 * ============================================================ */

static void Stage_dealloc(PyObject* self) {
  LightUSDStage* s = (LightUSDStage*)self;
  PyTypeObject* tp = Py_TYPE(self);
  if (s->stage) {
    lightusd_stage_destroy(s->stage);
    s->stage = NULL;
  }
  freefunc free_fn = (freefunc)PyType_GetSlot(tp, Py_tp_free);
  free_fn(self);
  Py_DECREF((PyObject*)tp);
}

static lightusd_state* stage_context(PyObject* self, lightusd_stage** out) {
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;
  lightusd_stage* stage = ((LightUSDStage*)self)->stage;
  if (!stage) {
    PyErr_SetString(st->UsdError, "Stage is closed");
    return NULL;
  }
  *out = stage;
  return st;
}

/* ---- lifecycle ---- */

static PyObject* Stage_create(PyObject* type_or_mod, PyObject* noargs) {
  (void)noargs;
  lightusd_state* st;
  if (PyType_Check(type_or_mod)) {
    st = lightusd_state_from_type((PyTypeObject*)type_or_mod);
  } else {
    st = lightusd_state_from_module(type_or_mod);
  }
  if (!st) return NULL;
  lightusd_stage* stage = NULL;
  lightusd_status status = lightusd_stage_create(&stage);
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, "Stage.create");
  return lightusd_wrap_stage(st, stage);
}

static PyObject* Stage_close(PyObject* self, PyObject* noargs) {
  (void)noargs;
  LightUSDStage* s = (LightUSDStage*)self;
  if (s->stage) {
    lightusd_stage_destroy(s->stage);
    s->stage = NULL;
  }
  Py_RETURN_NONE;
}

static PyObject* Stage_enter(PyObject* self, PyObject* noargs) {
  (void)noargs;
  Py_INCREF(self);
  return self;
}

static PyObject* Stage_exit(PyObject* self, PyObject* args) {
  (void)args;
  return Stage_close(self, NULL);
}

/* ---- prim access ---- */

static PyObject* Stage_prim_at(PyObject* self, PyObject* args) {
  const char* path;
  if (!PyArg_ParseTuple(args, "s:prim_at", &path)) return NULL;
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return NULL;
  lightusd_prim prim = lightusd_stage_prim_at_path(stage, path);
  if (!lightusd_prim_is_valid(prim)) {
    PyErr_Format(PyExc_KeyError, "no prim at path '%s'", path);
    return NULL;
  }
  return lightusd_wrap_prim(st, self, prim);
}

static PyObject* Stage_get_prim_at(PyObject* self, PyObject* args,
                                   PyObject* kwargs) {
  static char* kwlist[] = {"path", "default", NULL};
  const char* path;
  PyObject* def = Py_None;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|O:get_prim_at", kwlist,
                                   &path, &def)) {
    return NULL;
  }
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return NULL;
  lightusd_prim prim = lightusd_stage_prim_at_path(stage, path);
  if (!lightusd_prim_is_valid(prim)) {
    Py_INCREF(def);
    return def;
  }
  return lightusd_wrap_prim(st, self, prim);
}

static int Stage_contains(PyObject* self, PyObject* key) {
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return -1;
  PyObject* tmp = NULL;
  const char* path = lightusd_utf8(key, &tmp);
  if (!path) return -1;
  lightusd_prim prim = lightusd_stage_prim_at_path(stage, path);
  Py_XDECREF(tmp);
  return lightusd_prim_is_valid(prim) ? 1 : 0;
}

static Py_ssize_t Stage_length(PyObject* self) {
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return -1;
  return (Py_ssize_t)lightusd_stage_prim_count(stage);
}

static PyObject* Stage_iter(PyObject* self) {
  lightusd_state* st = lightusd_state_from_obj(self);
  if (!st) return NULL;
  return stage_iter_new(st, self);
}

static PyObject* Stage_get_root_prims(PyObject* self, void* closure) {
  (void)closure;
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return NULL;
  size_t n = lightusd_stage_root_prim_count(stage);
  PyObject* tup = PyTuple_New((Py_ssize_t)n);
  if (!tup) return NULL;
  for (size_t i = 0; i < n; ++i) {
    PyObject* prim = lightusd_wrap_prim(st, self, lightusd_stage_root_prim(stage, i));
    if (!prim) {
      Py_DECREF(tup);
      return NULL;
    }
    PyTuple_SetItem(tup, (Py_ssize_t)i, prim);
  }
  return tup;
}

static PyObject* Stage_get_pseudo_root(PyObject* self, void* closure) {
  (void)closure;
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return NULL;
  return lightusd_wrap_prim(st, self, lightusd_stage_pseudo_root(stage));
}

static PyObject* Stage_get_default_prim(PyObject* self, void* closure) {
  (void)closure;
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return NULL;
  return lightusd_wrap_prim(st, self, lightusd_stage_default_prim(stage));
}

static PyObject* Stage_prims_of_type(PyObject* self, PyObject* args) {
  const char* type_name;
  if (!PyArg_ParseTuple(args, "s:prims_of_type", &type_name)) return NULL;
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return NULL;

  PyObject* list = PyList_New(0);
  if (!list) return NULL;
  PyObject* iter = stage_iter_new(st, self);
  if (!iter) {
    Py_DECREF(list);
    return NULL;
  }
  const size_t want_len = strlen(type_name);
  PyObject* prim_obj;
  while ((prim_obj = StageIter_next(iter)) != NULL) {
    lightusd_sv tn = lightusd_prim_type_name(((LightUSDPrim*)prim_obj)->prim);
    if (tn.len == want_len && strncmp(tn.data, type_name, tn.len) == 0) {
      if (PyList_Append(list, prim_obj) != 0) {
        Py_DECREF(prim_obj);
        Py_DECREF(iter);
        Py_DECREF(list);
        return NULL;
      }
    }
    Py_DECREF(prim_obj);
  }
  Py_DECREF(iter);
  if (PyErr_Occurred()) {
    Py_DECREF(list);
    return NULL;
  }
  return list;
}

/* ---- metadata ---- */

static PyObject* Stage_get_metadata(PyObject* self, PyObject* args) {
  const char* key;
  if (!PyArg_ParseTuple(args, "s:get_metadata", &key)) return NULL;
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return NULL;
  lightusd_value* val = NULL;
  lightusd_status status = lightusd_stage_get_metadata(stage, key, &val);
  if (status == LIGHTUSD_ERR_NOT_FOUND) Py_RETURN_NONE;
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, key);
  return lightusd_value_to_python(st, val);
}

static PyObject* Stage_set_metadata(PyObject* self, PyObject* args) {
  const char* key;
  PyObject* value;
  if (!PyArg_ParseTuple(args, "sO:set_metadata", &key, &value)) return NULL;
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return NULL;

  lightusd_status status;
  if (PyUnicode_Check(value)) {
    PyObject* tmp = NULL;
    const char* s = lightusd_utf8(value, &tmp);
    if (!s) return NULL;
    status = lightusd_stage_set_metadata(stage, key, LIGHTUSD_TYPE_TOKEN, s, 1);
    Py_XDECREF(tmp);
  } else if (PyBool_Check(value)) {
    uint8_t b = value == Py_True ? 1 : 0;
    status = lightusd_stage_set_metadata(stage, key, LIGHTUSD_TYPE_BOOL, &b, 1);
  } else if (PyLong_Check(value) || PyFloat_Check(value)) {
    double d = PyFloat_AsDouble(value);
    if (d == -1.0 && PyErr_Occurred()) return NULL;
    status = lightusd_stage_set_metadata(stage, key, LIGHTUSD_TYPE_DOUBLE, &d, 1);
  } else {
    PyErr_SetString(PyExc_TypeError,
                    "stage metadata value must be str, bool, int or float");
    return NULL;
  }
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, key);
  Py_RETURN_NONE;
}

/* Metadata-backed float properties (closure = key). */
static PyObject* Stage_get_meta_prop(PyObject* self, void* closure) {
  const char* key = (const char*)closure;
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return NULL;
  lightusd_value* val = NULL;
  lightusd_status status = lightusd_stage_get_metadata(stage, key, &val);
  if (status != LIGHTUSD_OK) Py_RETURN_NONE;
  return lightusd_value_to_python(st, val);
}

static int Stage_set_meta_prop(PyObject* self, PyObject* value,
                               void* closure) {
  PyObject* args = Py_BuildValue("(sO)", (const char*)closure, value);
  if (!args) return -1;
  PyObject* res = Stage_set_metadata(self, args);
  Py_DECREF(args);
  if (!res) return -1;
  Py_DECREF(res);
  return 0;
}

static PyObject* Stage_get_default_prim_path(PyObject* self, void* closure) {
  (void)closure;
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return NULL;
  lightusd_sv sv = lightusd_stage_default_prim_path(stage);
  if (sv.len == 0) Py_RETURN_NONE;
  return lightusd_sv_to_str(sv);
}

static PyObject* Stage_set_default_prim(PyObject* self, PyObject* args) {
  const char* name;
  if (!PyArg_ParseTuple(args, "s:set_default_prim", &name)) return NULL;
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return NULL;
  lightusd_status status = lightusd_stage_set_default_prim(stage, name);
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, name);
  Py_RETURN_NONE;
}

static PyObject* Stage_get_sublayers(PyObject* self, void* closure) {
  (void)closure;
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return NULL;
  lightusd_strlist* subs = NULL;
  if (lightusd_stage_sublayers(stage, &subs) != LIGHTUSD_OK) {
    return lightusd_raise(st, LIGHTUSD_ERR_INTERNAL, "sublayers");
  }
  size_t n = lightusd_strlist_size(subs);
  PyObject* tup = PyTuple_New((Py_ssize_t)n);
  if (!tup) {
    lightusd_strlist_destroy(subs);
    return NULL;
  }
  for (size_t i = 0; i < n; ++i) {
    PyObject* s = lightusd_sv_to_str(lightusd_strlist_get(subs, i));
    if (!s) {
      Py_DECREF(tup);
      lightusd_strlist_destroy(subs);
      return NULL;
    }
    PyTuple_SetItem(tup, (Py_ssize_t)i, s);
  }
  lightusd_strlist_destroy(subs);
  return tup;
}

static PyObject* Stage_add_sublayer(PyObject* self, PyObject* args) {
  const char* path;
  if (!PyArg_ParseTuple(args, "s:add_sublayer", &path)) return NULL;
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return NULL;
  lightusd_status status = lightusd_stage_add_sublayer_path(stage, path);
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, path);
  Py_RETURN_NONE;
}

static PyObject* Stage_get_custom_layer_data(PyObject* self, void* closure) {
  (void)closure;
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return NULL;
  lightusd_dict_ref dict;
  if (lightusd_stage_custom_layer_data(stage, &dict) != LIGHTUSD_OK) {
    dict._dict = NULL;
  }
  return lightusd_dict_to_python(st, dict);
}

static PyObject* Stage_get_stats(PyObject* self, void* closure) {
  (void)closure;
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return NULL;
  lightusd_stage_stats stats;
  lightusd_status status = lightusd_stage_get_stats(stage, &stats);
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, "stats");
  return Py_BuildValue(
      "{s:K,s:K,s:K,s:K}", "prim_count",
      (unsigned long long)stats.prim_count, "layer_count",
      (unsigned long long)stats.layer_count, "total_properties",
      (unsigned long long)stats.total_properties, "memory_bytes",
      (unsigned long long)stats.memory_bytes);
}

static PyObject* Stage_warnings(PyObject* self, PyObject* noargs) {
  (void)noargs;
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return NULL;
  lightusd_string* s = NULL;
  if (lightusd_stage_take_warnings(stage, &s) != LIGHTUSD_OK) {
    return lightusd_raise(st, LIGHTUSD_ERR_INTERNAL, "warnings");
  }
  lightusd_sv sv = lightusd_string_view(s);
  PyObject* res = lightusd_sv_to_str(sv);
  lightusd_string_destroy(s);
  return res;
}

/* ---- authoring ---- */

static PyObject* Stage_define_prim(PyObject* self, PyObject* args,
                                   PyObject* kwargs) {
  static char* kwlist[] = {"path", "type_name", "specifier", NULL};
  const char* path;
  const char* type_name = NULL;
  const char* specifier = "def";
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|zs:define_prim", kwlist,
                                   &path, &type_name, &specifier)) {
    return NULL;
  }
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return NULL;

  uint8_t spec;
  if (strcmp(specifier, "def") == 0) {
    spec = 0;
  } else if (strcmp(specifier, "over") == 0) {
    spec = 1;
  } else if (strcmp(specifier, "class") == 0) {
    spec = 2;
  } else {
    PyErr_SetString(PyExc_ValueError,
                    "specifier must be 'def', 'over' or 'class'");
    return NULL;
  }
  lightusd_prim prim;
  lightusd_status status =
      lightusd_stage_define_prim(stage, path, type_name, spec, &prim);
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, path);
  return lightusd_wrap_prim(st, self, prim);
}

static PyObject* Stage_override_prim(PyObject* self, PyObject* args) {
  const char* path;
  if (!PyArg_ParseTuple(args, "s:override_prim", &path)) return NULL;
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return NULL;
  lightusd_prim prim;
  lightusd_status status = lightusd_stage_define_prim(stage, path, NULL, 1, &prim);
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, path);
  return lightusd_wrap_prim(st, self, prim);
}

static PyObject* Stage_remove_prim(PyObject* self, PyObject* args) {
  const char* path;
  if (!PyArg_ParseTuple(args, "s:remove_prim", &path)) return NULL;
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return NULL;
  lightusd_status status = lightusd_stage_remove_prim(stage, path);
  if (status == LIGHTUSD_ERR_NOT_FOUND) {
    PyErr_Format(PyExc_KeyError, "no prim at path '%s'", path);
    return NULL;
  }
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, path);
  Py_RETURN_NONE;
}

/* ---- serialization ---- */

static PyObject* Stage_save(PyObject* self, PyObject* args, PyObject* kwargs) {
  static char* kwlist[] = {"path", "format", NULL};
  const char* path;
  const char* format = NULL;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|z:save", kwlist, &path,
                                   &format)) {
    return NULL;
  }
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return NULL;

  lightusd_save_options opts;
  lightusd_save_options_init(&opts);
  if (format) {
    if (strcmp(format, "usda") == 0) {
      opts.format = LIGHTUSD_FORMAT_USDA;
    } else if (strcmp(format, "usdc") == 0) {
      opts.format = LIGHTUSD_FORMAT_USDC;
    } else if (strcmp(format, "usdz") == 0) {
      opts.format = LIGHTUSD_FORMAT_USDZ;
    } else {
      PyErr_SetString(PyExc_ValueError,
                      "format must be 'usda', 'usdc' or 'usdz'");
      return NULL;
    }
  }
  lightusd_status status;
  Py_BEGIN_ALLOW_THREADS
  status = lightusd_stage_save(stage, path, &opts);
  Py_END_ALLOW_THREADS
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, path);
  Py_RETURN_NONE;
}

static PyObject* Stage_save_usdz(PyObject* self, PyObject* args,
                                 PyObject* kwargs) {
  static char* kwlist[] = {"path", "assets", NULL};
  const char* path;
  PyObject* assets_obj = Py_None;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|O:save_usdz", kwlist,
                                   &path, &assets_obj)) return NULL;
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return NULL;
  PyObject* items = NULL;
  Py_ssize_t count = 0;
  if (assets_obj != Py_None) {
    if (!PyDict_Check(assets_obj)) {
      PyErr_SetString(PyExc_TypeError, "assets must be a dict of path: bytes");
      return NULL;
    }
    count = PyDict_Size(assets_obj);
    items = PyDict_Items(assets_obj);
    if (!items) return NULL;
  }
  const char** names = (const char**)PyMem_Calloc(
      (size_t)count ? (size_t)count : 1, sizeof(*names));
  const uint8_t** data = (const uint8_t**)PyMem_Calloc(
      (size_t)count ? (size_t)count : 1, sizeof(*data));
  size_t* sizes = (size_t*)PyMem_Calloc(
      (size_t)count ? (size_t)count : 1, sizeof(*sizes));
  PyObject** name_refs = (PyObject**)PyMem_Calloc(
      (size_t)count ? (size_t)count : 1, sizeof(*name_refs));
  if (!names || !data || !sizes || !name_refs) {
    Py_XDECREF(items);
    PyMem_Free(names); PyMem_Free(data); PyMem_Free(sizes); PyMem_Free(name_refs);
    return PyErr_NoMemory();
  }
  int ok = 1;
  for (Py_ssize_t i = 0; i < count; ++i) {
    PyObject* pair = PyList_GetItem(items, i);
    PyObject* name = PyTuple_GetItem(pair, 0);
    PyObject* bytes = PyTuple_GetItem(pair, 1);
    if (!PyUnicode_Check(name) || !PyBytes_Check(bytes)) {
      PyErr_SetString(PyExc_TypeError, "USDZ assets require str keys and bytes values");
      ok = 0;
      break;
    }
    name_refs[i] = PyUnicode_AsUTF8String(name);
    if (!name_refs[i]) { ok = 0; break; }
    names[i] = PyBytes_AsString(name_refs[i]);
    char* raw = NULL;
    Py_ssize_t raw_size = 0;
    if (PyBytes_AsStringAndSize(bytes, &raw, &raw_size) != 0) { ok = 0; break; }
    data[i] = (const uint8_t*)raw;
    sizes[i] = (size_t)raw_size;
  }
  lightusd_status status = LIGHTUSD_ERR_INVALID_ARG;
  if (ok) {
    Py_BEGIN_ALLOW_THREADS
    status = lightusd_stage_save_usdz_with_assets(
        stage, path, names, data, sizes, (size_t)count);
    Py_END_ALLOW_THREADS
  }
  Py_XDECREF(items);
  for (Py_ssize_t i = 0; i < count; ++i) Py_XDECREF(name_refs[i]);
  PyMem_Free(names); PyMem_Free(data); PyMem_Free(sizes); PyMem_Free(name_refs);
  if (!ok) return NULL;
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, path);
  Py_RETURN_NONE;
}

static PyObject* Stage_export_usda(PyObject* self, PyObject* noargs) {
  (void)noargs;
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return NULL;
  lightusd_string* s = NULL;
  lightusd_status status;
  Py_BEGIN_ALLOW_THREADS
  status = lightusd_stage_export_usda(stage, &s);
  Py_END_ALLOW_THREADS
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, "export_usda");
  lightusd_sv sv = lightusd_string_view(s);
  PyObject* res = lightusd_sv_to_str(sv);
  lightusd_string_destroy(s);
  return res;
}

static PyObject* Stage_export_usdc(PyObject* self, PyObject* noargs) {
  (void)noargs;
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return NULL;
  lightusd_string* s = NULL;
  lightusd_status status;
  Py_BEGIN_ALLOW_THREADS
  status = lightusd_stage_export_usdc(stage, &s);
  Py_END_ALLOW_THREADS
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, "export_usdc");
  lightusd_sv sv = lightusd_string_view(s);
  PyObject* res = PyBytes_FromStringAndSize(sv.data, (Py_ssize_t)sv.len);
  lightusd_string_destroy(s);
  return res;
}

static PyObject* Stage_flattened(PyObject* self, PyObject* noargs) {
  (void)noargs;
  lightusd_stage* stage;
  lightusd_state* st = stage_context(self, &stage);
  if (!st) return NULL;
  lightusd_stage* flat = NULL;
  lightusd_status status;
  Py_BEGIN_ALLOW_THREADS
  status = lightusd_stage_flatten(stage, &flat);
  Py_END_ALLOW_THREADS
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, "flattened");
  return lightusd_wrap_stage(st, flat);
}

static PyObject* Stage_repr(PyObject* self) {
  LightUSDStage* s = (LightUSDStage*)self;
  if (!s->stage) return PyUnicode_FromString("Stage(<closed>)");
  return PyUnicode_FromFormat("Stage(prims=%zu)",
                              lightusd_stage_prim_count(s->stage));
}

static PyMethodDef Stage_methods[] = {
    {"create", Stage_create, METH_NOARGS | METH_CLASS,
     "Create an empty stage for authoring."},
    {"close", Stage_close, METH_NOARGS,
     "Release the underlying C stage immediately."},
    {"__enter__", Stage_enter, METH_NOARGS, NULL},
    {"__exit__", Stage_exit, METH_VARARGS, NULL},
    {"prim_at", Stage_prim_at, METH_VARARGS,
     "prim_at(path) -> Prim (raises KeyError)"},
    {"get_prim_at", (PyCFunction)(void (*)(void))Stage_get_prim_at,
     METH_VARARGS | METH_KEYWORDS,
     "get_prim_at(path, default=None) -> Prim | default"},
    {"prims_of_type", Stage_prims_of_type, METH_VARARGS,
     "prims_of_type(type_name) -> list[Prim]"},
    {"get_metadata", Stage_get_metadata, METH_VARARGS,
     "get_metadata(key) -> value | None"},
    {"set_metadata", Stage_set_metadata, METH_VARARGS,
     "set_metadata(key, value)"},
    {"set_default_prim", Stage_set_default_prim, METH_VARARGS,
     "set_default_prim(prim_name)"},
    {"add_sublayer", Stage_add_sublayer, METH_VARARGS,
     "add_sublayer(asset_path)"},
    {"warnings", Stage_warnings, METH_NOARGS,
     "Drain accumulated load warnings."},
    {"define_prim", (PyCFunction)(void (*)(void))Stage_define_prim,
     METH_VARARGS | METH_KEYWORDS,
     "define_prim(path, type_name=None, *, specifier='def') -> Prim"},
    {"override_prim", Stage_override_prim, METH_VARARGS,
     "override_prim(path) -> Prim"},
    {"remove_prim", Stage_remove_prim, METH_VARARGS, "remove_prim(path)"},
    {"save", (PyCFunction)(void (*)(void))Stage_save,
     METH_VARARGS | METH_KEYWORDS,
     "save(path, *, format=None)\nWrite USDA/USDC/USDZ (by extension)."},
    {"save_usdz", (PyCFunction)(void (*)(void))Stage_save_usdz,
     METH_VARARGS | METH_KEYWORDS,
     "save_usdz(path, assets=None)\nWrite a USDZ with package assets."},
    {"export_usda", Stage_export_usda, METH_NOARGS,
     "Serialize to a USDA string."},
    {"export_usdc", Stage_export_usdc, METH_NOARGS,
     "Serialize to USDC (crate) bytes."},
    {"flattened", Stage_flattened, METH_NOARGS,
     "Flatten composition into a new Stage."},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef Stage_getset[] = {
    {"root_prims", Stage_get_root_prims, NULL, "Root prims (tuple).", NULL},
    {"pseudo_root", Stage_get_pseudo_root, NULL, "Pseudo-root prim.", NULL},
    {"default_prim", Stage_get_default_prim, NULL,
     "Default prim (or None).", NULL},
    {"default_prim_path", Stage_get_default_prim_path, NULL,
     "defaultPrim name (or None).", NULL},
    {"up_axis", Stage_get_meta_prop, Stage_set_meta_prop, "upAxis token.",
     (void*)"upAxis"},
    {"meters_per_unit", Stage_get_meta_prop, Stage_set_meta_prop,
     "metersPerUnit.", (void*)"metersPerUnit"},
    {"time_codes_per_second", Stage_get_meta_prop, Stage_set_meta_prop,
     "timeCodesPerSecond.", (void*)"timeCodesPerSecond"},
    {"start_time", Stage_get_meta_prop, Stage_set_meta_prop,
     "startTimeCode.", (void*)"startTimeCode"},
    {"end_time", Stage_get_meta_prop, Stage_set_meta_prop, "endTimeCode.",
     (void*)"endTimeCode"},
    {"frames_per_second", Stage_get_meta_prop, Stage_set_meta_prop,
     "framesPerSecond.", (void*)"framesPerSecond"},
    {"doc", Stage_get_meta_prop, Stage_set_meta_prop, "Stage doc string.",
     (void*)"doc"},
    {"sublayers", Stage_get_sublayers, NULL, "Sublayer asset paths.", NULL},
    {"custom_layer_data", Stage_get_custom_layer_data, NULL,
     "customLayerData dictionary (copied).", NULL},
    {"stats", Stage_get_stats, NULL, "Stage statistics (dict).", NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyType_Slot Stage_slots[] = {
    {Py_tp_dealloc, (void*)Stage_dealloc},
    {Py_tp_repr, (void*)Stage_repr},
    {Py_tp_iter, (void*)Stage_iter},
    {Py_sq_length, (void*)Stage_length},
    {Py_sq_contains, (void*)Stage_contains},
    {Py_tp_methods, (void*)Stage_methods},
    {Py_tp_getset, (void*)Stage_getset},
    {0, NULL},
};

static PyType_Spec Stage_spec = {
    .name = "lightusd._core.Stage",
    .basicsize = sizeof(LightUSDStage),
    .flags = Py_TPFLAGS_DEFAULT
#ifdef Py_TPFLAGS_IMMUTABLETYPE
             | Py_TPFLAGS_IMMUTABLETYPE
#endif
    ,
    .slots = Stage_slots,
};

int lightusd_register_stage_type(PyObject* module, lightusd_state* st) {
  st->StageType = PyType_FromModuleAndSpec(module, &Stage_spec, NULL);
  if (!st->StageType) return -1;
  if (PyModule_AddObjectRef(module, "Stage", st->StageType) < 0) return -1;
  st->StageIterType = PyType_FromModuleAndSpec(module, &StageIter_spec, NULL);
  if (!st->StageIterType) return -1;
  return 0;
}

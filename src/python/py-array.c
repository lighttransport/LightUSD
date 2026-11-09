/* SPDX-License-Identifier: Apache-2.0
 * lightusd._core — Array type (zero-copy view) and value conversion helpers.
 */

#include "py-internal.h"

/* ============================================================
 * Small helpers
 * ============================================================ */

/* Read component `i` of the buffer as a Python scalar. */
static PyObject* component_to_python(const void* data, uint8_t storage,
                                     size_t i, int as_bool) {
  switch (storage) {
    case LIGHTUSD_COMP_UINT8: {
      uint8_t v = ((const uint8_t*)data)[i];
      if (as_bool) return PyBool_FromLong(v != 0);
      return PyLong_FromLong(v);
    }
    case LIGHTUSD_COMP_INT32:
      return PyLong_FromLong(((const int32_t*)data)[i]);
    case LIGHTUSD_COMP_UINT32:
      return PyLong_FromUnsignedLong(((const uint32_t*)data)[i]);
    case LIGHTUSD_COMP_INT64:
      return PyLong_FromLongLong(((const int64_t*)data)[i]);
    case LIGHTUSD_COMP_UINT64:
      return PyLong_FromUnsignedLongLong(((const uint64_t*)data)[i]);
    case LIGHTUSD_COMP_FLOAT16:
      return PyFloat_FromDouble(
          (double)lu_half_to_float(((const uint16_t*)data)[i]));
    case LIGHTUSD_COMP_FLOAT32:
      return PyFloat_FromDouble((double)((const float*)data)[i]);
    case LIGHTUSD_COMP_FLOAT64:
      return PyFloat_FromDouble(((const double*)data)[i]);
    case LIGHTUSD_COMP_UINT16:
      return PyLong_FromLong(((const uint16_t*)data)[i]);
    case LIGHTUSD_COMP_INT16:
      return PyLong_FromLong(((const int16_t*)data)[i]);
    default:
      PyErr_SetString(PyExc_TypeError, "unsupported component storage");
      return NULL;
  }
}

/* Build a Python object for element `idx` of a view (scalar or tuple). */
static PyObject* element_to_python(const lightusd_value_view* view,
                                   Py_ssize_t idx) {
  const int as_bool = (view->type == LIGHTUSD_TYPE_BOOL);
  const size_t comps = view->components ? view->components : 1;
  if (idx < 0) {
    return NULL;
  }
  if (comps > 1) {
    size_t uidx = (size_t)idx;
    if (uidx > (size_t)-1 / comps) {
      return NULL;
    }
  }
  const size_t base = (size_t)idx * comps;
  if (comps == 1) {
    return component_to_python(view->data, view->storage, base, as_bool);
  }
  PyObject* tup = PyTuple_New((Py_ssize_t)comps);
  if (!tup) return NULL;
  for (size_t c = 0; c < comps; ++c) {
    PyObject* item =
        component_to_python(view->data, view->storage, base + c, as_bool);
    if (!item) {
      Py_DECREF(tup);
      return NULL;
    }
    PyTuple_SetItem(tup, (Py_ssize_t)c, item);
  }
  return tup;
}

/* ============================================================
 * Array type
 * ============================================================ */

static void Array_dealloc(PyObject* self) {
  LightUSDArray* a = (LightUSDArray*)self;
  PyTypeObject* tp = Py_TYPE(self);
  lightusd_buffer_destroy(a->buffer);
  freefunc free_fn = (freefunc)PyType_GetSlot(tp, Py_tp_free);
  free_fn(self);
  Py_DECREF((PyObject*)tp);
}

static Py_ssize_t Array_length(PyObject* self) {
  return (Py_ssize_t)((LightUSDArray*)self)->view.count;
}

static PyObject* Array_getitem(PyObject* self, Py_ssize_t idx) {
  LightUSDArray* a = (LightUSDArray*)self;
  Py_ssize_t n = (Py_ssize_t)a->view.count;
  if (idx < 0) idx += n;
  if (idx < 0 || idx >= n) {
    PyErr_SetString(PyExc_IndexError, "Array index out of range");
    return NULL;
  }
  return element_to_python(&a->view, idx);
}

static PyObject* Array_get_shape(PyObject* self, void* closure) {
  LightUSDArray* a = (LightUSDArray*)self;
  (void)closure;
  if (a->ndim == 2) {
    return Py_BuildValue("(nn)", a->shape[0], a->shape[1]);
  }
  return Py_BuildValue("(n)", a->shape[0]);
}

static PyObject* Array_get_dtype(PyObject* self, void* closure) {
  LightUSDArray* a = (LightUSDArray*)self;
  (void)closure;
  const lu_component_info* info = lightusd_component_get(a->view.storage);
  return PyUnicode_FromString(info ? info->name : "void");
}

static PyObject* Array_get_type_name(PyObject* self, void* closure) {
  LightUSDArray* a = (LightUSDArray*)self;
  (void)closure;
  return PyUnicode_FromString(lightusd_type_name(a->view.type));
}

static PyObject* Array_get_nbytes(PyObject* self, void* closure) {
  (void)closure;
  return PyLong_FromSize_t(((LightUSDArray*)self)->view.nbytes);
}

static PyObject* Array_tolist(PyObject* self, PyObject* noargs) {
  LightUSDArray* a = (LightUSDArray*)self;
  (void)noargs;
  Py_ssize_t n = (Py_ssize_t)a->view.count;
  PyObject* list = PyList_New(n);
  if (!list) return NULL;
  for (Py_ssize_t i = 0; i < n; ++i) {
    PyObject* item = element_to_python(&a->view, i);
    if (!item) {
      Py_DECREF(list);
      return NULL;
    }
    PyList_SetItem(list, i, item);
  }
  return list;
}

static PyObject* Array_repr(PyObject* self) {
  LightUSDArray* a = (LightUSDArray*)self;
  PyObject* dtype = Array_get_dtype(self, NULL);
  if (!dtype) return NULL;
  PyObject* res;
  if (a->ndim == 2) {
    res = PyUnicode_FromFormat("Array(%U, shape=(%zd, %zd))", dtype,
                               a->shape[0], a->shape[1]);
  } else {
    res = PyUnicode_FromFormat("Array(%U, shape=(%zd,))", dtype, a->shape[0]);
  }
  Py_DECREF(dtype);
  return res;
}

/* Buffer protocol for both abi3 and free-threaded builds: np.asarray
 * and memoryview(arr) work natively with dtype + 2-D shape. All fields are
 * immutable after creation, so no locking is needed. */
static int Array_getbuffer(PyObject* self, Py_buffer* buf, int flags) {
  LightUSDArray* a = (LightUSDArray*)self;
  if ((flags & PyBUF_WRITABLE) == PyBUF_WRITABLE) {
    PyErr_SetString(PyExc_BufferError, "Array is read-only");
    return -1;
  }
  if ((flags & PyBUF_F_CONTIGUOUS) == PyBUF_F_CONTIGUOUS &&
      a->ndim == 2 && a->shape[0] > 1 && a->shape[1] > 1) {
    PyErr_SetString(PyExc_BufferError, "Array is C contiguous");
    return -1;
  }
  memset(buf, 0, sizeof(*buf));
  buf->buf = (void*)a->view.data;
  buf->obj = self;
  Py_INCREF(self);
  buf->len = (Py_ssize_t)a->view.nbytes;
  buf->readonly = 1;
  buf->itemsize = a->itemsize;
  if ((flags & PyBUF_FORMAT) == PyBUF_FORMAT) {
    buf->format = a->format;
  }
  if ((flags & PyBUF_ND) == PyBUF_ND) {
    buf->ndim = a->ndim;
    buf->shape = a->shape;
  } else {
    buf->ndim = 1;
  }
  if ((flags & PyBUF_STRIDES) == PyBUF_STRIDES) buf->strides = a->strides;
  if (!(flags & PyBUF_ND) && !(flags & PyBUF_FORMAT)) buf->itemsize = 1;
  return 0;
}

static PyMethodDef Array_methods[] = {
    {"tolist", Array_tolist, METH_NOARGS,
     "Copy the contents into a list of Python scalars/tuples."},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef Array_getset[] = {
    {"shape", Array_get_shape, NULL, "Logical shape.", NULL},
    {"dtype", Array_get_dtype, NULL, "numpy-style dtype name.", NULL},
    {"type_name", Array_get_type_name, NULL, "USD element type name.", NULL},
    {"nbytes", Array_get_nbytes, NULL, "Total byte size.", NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyType_Slot Array_slots[] = {
    {Py_tp_dealloc, (void*)Array_dealloc},
    {Py_tp_repr, (void*)Array_repr},
    {Py_sq_length, (void*)Array_length},
    {Py_sq_item, (void*)Array_getitem},
    {Py_tp_methods, (void*)Array_methods},
    {Py_tp_getset, (void*)Array_getset},
    {Py_bf_getbuffer, (void*)Array_getbuffer},
    {0, NULL},
};

static PyType_Spec Array_spec = {
    .name = "lightusd._core.Array",
    .basicsize = sizeof(LightUSDArray),
    .flags = Py_TPFLAGS_DEFAULT
#ifdef Py_TPFLAGS_IMMUTABLETYPE
             | Py_TPFLAGS_IMMUTABLETYPE
#endif
    ,
    .slots = Array_slots,
};

int lightusd_register_array_type(PyObject* module, lightusd_state* st) {
  st->ArrayType = PyType_FromModuleAndSpec(module, &Array_spec, NULL);
  if (!st->ArrayType) return -1;
  return PyModule_AddObjectRef(module, "Array", st->ArrayType);
}

/* ============================================================
 * Array construction
 * ============================================================ */

PyObject* lightusd_wrap_array_pin(lightusd_state* st, lightusd_buffer* buffer) {
  const lightusd_value_view pinned = lightusd_buffer_get_view(buffer);
  const lightusd_value_view* view = &pinned;
  const lu_component_info* info = lightusd_component_get(view->storage);
  if (!info || !info->size || view->count > PY_SSIZE_T_MAX || view->nbytes > PY_SSIZE_T_MAX) {
    lightusd_buffer_destroy(buffer);
    PyErr_SetString(st->UsdError, "array has an unsupported layout"); return NULL;
  }
  PyObject* obj = lightusd_alloc(st->ArrayType);
  if (!obj) { lightusd_buffer_destroy(buffer); return NULL; }
  LightUSDArray* a = (LightUSDArray*)obj;
  a->buffer = buffer;
  a->view = lightusd_buffer_get_view(buffer);
  a->format[0] = info->format;
  a->format[1] = '\0';
  a->itemsize = info->size;
  if (view->components > 1) {
    a->ndim = 2;
    a->shape[0] = (Py_ssize_t)view->count;
    a->shape[1] = (Py_ssize_t)view->components;
  } else {
    a->ndim = 1;
    a->shape[0] = (Py_ssize_t)view->count;
    a->shape[1] = 0;
  }
  a->strides[0] = a->itemsize * (a->ndim == 2 ? a->shape[1] : 1);
  a->strides[1] = a->itemsize;
  return obj;
}

static PyObject* array_new_common(lightusd_state* st, PyObject* owner,
    lightusd_value* owned, const lightusd_value_view* view) {
  lightusd_buffer* buffer = NULL;
  lightusd_status status = lightusd_buffer_pin(*view, &buffer);
  (void)owner;
  lightusd_value_destroy(owned);
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, "pin array");
  return lightusd_wrap_array_pin(st, buffer);
}

PyObject* lightusd_wrap_array_borrowed(lightusd_state* st, PyObject* owner,
                                   const lightusd_value_view* view) {
  return array_new_common(st, owner, NULL, view);
}

PyObject* lightusd_wrap_array_owned(lightusd_state* st, lightusd_value* owned,
                                const lightusd_value_view* view) {
  return array_new_common(st, NULL, owned, view);
}

/* ============================================================
 * View / value conversion
 * ============================================================ */

PyObject* lightusd_view_to_python(lightusd_state* st, PyObject* owner,
                              lightusd_value* owned, const lightusd_value_view* view) {
  if (view->is_block) {
    if (owned) lightusd_value_destroy(owned);
    Py_INCREF(st->ValueBlock);
    return st->ValueBlock;
  }
  if (view->is_array) {
    if (!view->data && view->count > 0) {
      if (owned) lightusd_value_destroy(owned);
      PyErr_Format(st->UsdError, "array value of type '%s' is not viewable",
                   lightusd_type_name(view->type));
      return NULL;
    }
    if (owned) {
      return lightusd_wrap_array_owned(st, owned, view);
    }
    return lightusd_wrap_array_borrowed(st, owner, view);
  }

  /* Scalar */
  if (!view->data) {
    if (owned) lightusd_value_destroy(owned);
    PyErr_Format(st->UsdError, "value of type '%s' has no data",
                 lightusd_type_name(view->type));
    return NULL;
  }
  PyObject* res = element_to_python(view, 0);
  if (owned) lightusd_value_destroy(owned);
  return res;
}

PyObject* lightusd_value_to_python(lightusd_state* st, lightusd_value* val) {
  lightusd_dict_ref dict = lightusd_value_get_dictionary(val);
  if (lightusd_dict_is_valid(dict)) {
    PyObject* result = lightusd_dict_to_python(st, dict);
    lightusd_value_destroy(val);
    return result;
  }
  /* Try string family first. */
  lightusd_sv sv;
  if (lightusd_value_get_string(val, &sv) == LIGHTUSD_OK) {
    PyObject* s = lightusd_sv_to_str(sv);
    lightusd_value_destroy(val);
    return s;
  }
  lightusd_strlist* tokens = NULL;
  if (lightusd_value_get_token_array(val, &tokens) == LIGHTUSD_OK) {
    size_t n = lightusd_strlist_size(tokens);
    PyObject* tup = PyTuple_New((Py_ssize_t)n);
    if (!tup) {
      lightusd_strlist_destroy(tokens);
      lightusd_value_destroy(val);
      return NULL;
    }
    for (size_t i = 0; i < n; ++i) {
      PyObject* item = lightusd_sv_to_str(lightusd_strlist_get(tokens, i));
      if (!item) {
        Py_DECREF(tup);
        lightusd_strlist_destroy(tokens);
        lightusd_value_destroy(val);
        return NULL;
      }
      PyTuple_SetItem(tup, (Py_ssize_t)i, item);
    }
    lightusd_strlist_destroy(tokens);
    lightusd_value_destroy(val);
    return tup;
  }

  lightusd_value_view view;
  lightusd_status status = lightusd_value_get_view(val, &view);
  if (status != LIGHTUSD_OK) {
    lightusd_value_destroy(val);
    return lightusd_raise(st, status, "value view");
  }
  return lightusd_view_to_python(st, NULL, val, &view);
}

PyObject* lightusd_dict_to_python(lightusd_state* st, lightusd_dict_ref dict) {
  PyObject* out = PyDict_New();
  if (!out) return NULL;
  if (!lightusd_dict_is_valid(dict)) return out;

  size_t n = lightusd_dict_size(dict);
  for (size_t i = 0; i < n; ++i) {
    lightusd_sv key, sval;
    lightusd_value_view view;
    lightusd_dict_ref sub;
    if (lightusd_dict_entry(dict, i, &key, &view, &sval, &sub) != LIGHTUSD_OK) {
      continue;
    }
    PyObject* pykey = lightusd_sv_to_str(key);
    if (!pykey) goto fail;
    PyObject* pyval = NULL;
    if (lightusd_dict_is_valid(sub)) {
      pyval = lightusd_dict_to_python(st, sub);
    } else if (sval.len > 0 || (view.data == NULL && !view.is_array &&
                                (view.type == LIGHTUSD_TYPE_STRING ||
                                 view.type == LIGHTUSD_TYPE_TOKEN ||
                                 view.type == LIGHTUSD_TYPE_ASSET_PATH))) {
      pyval = lightusd_sv_to_str(sval);
    } else if (view.data) {
      /* Copy PODs out of the dict (dict values are small metadata). Arrays
       * become lists here rather than zero-copy views. */
      if (view.is_array) {
        PyObject* list = PyList_New((Py_ssize_t)view.count);
        if (list) {
          for (Py_ssize_t k = 0; k < (Py_ssize_t)view.count; ++k) {
            PyObject* item = element_to_python(&view, k);
            if (!item) {
              Py_CLEAR(list);
              break;
            }
            PyList_SetItem(list, k, item);
          }
        }
        pyval = list;
      } else {
        pyval = element_to_python(&view, 0);
      }
    } else {
      pyval = Py_None;
      Py_INCREF(Py_None);
    }
    if (!pyval) {
      Py_DECREF(pykey);
      goto fail;
    }
    int rc = PyDict_SetItem(out, pykey, pyval);
    Py_DECREF(pykey);
    Py_DECREF(pyval);
    if (rc != 0) goto fail;
  }
  return out;

fail:
  Py_DECREF(out);
  return NULL;
}

PyObject* lightusd_attr_value_to_python(lightusd_state* st, PyObject* stage_obj,
                                    lightusd_prim prim, const char* name) {
  lightusd_value_view view;
  lightusd_status status = lightusd_attr_get(prim, name, &view);
  if (status != LIGHTUSD_OK) {
    if (status == LIGHTUSD_ERR_NOT_FOUND) {
      PyErr_Format(PyExc_KeyError, "%s", name);
      return NULL;
    }
    return lightusd_raise(st, status, name);
  }

  if (!view.is_array && (view.type == LIGHTUSD_TYPE_STRING ||
                         view.type == LIGHTUSD_TYPE_TOKEN ||
                         view.type == LIGHTUSD_TYPE_ASSET_PATH)) {
    lightusd_sv sv;
    status = lightusd_attr_get_string(prim, name, &sv);
    if (status != LIGHTUSD_OK) return lightusd_raise(st, status, name);
    return lightusd_sv_to_str(sv);
  }

  if (view.is_array && (view.type == LIGHTUSD_TYPE_TOKEN ||
                        view.type == LIGHTUSD_TYPE_STRING ||
                        view.type == LIGHTUSD_TYPE_ASSET_PATH)) {
    lightusd_strlist* tokens = NULL;
    status = lightusd_attr_get_token_array(prim, name, &tokens);
    if (status != LIGHTUSD_OK) return lightusd_raise(st, status, name);
    size_t n = lightusd_strlist_size(tokens);
    PyObject* tup = PyTuple_New((Py_ssize_t)n);
    if (!tup) {
      lightusd_strlist_destroy(tokens);
      return NULL;
    }
    for (size_t i = 0; i < n; ++i) {
      PyObject* item = lightusd_sv_to_str(lightusd_strlist_get(tokens, i));
      if (!item) {
        Py_DECREF(tup);
        lightusd_strlist_destroy(tokens);
        return NULL;
      }
      PyTuple_SetItem(tup, (Py_ssize_t)i, item);
    }
    lightusd_strlist_destroy(tokens);
    return tup;
  }

  return lightusd_view_to_python(st, stage_obj, NULL, &view);
}

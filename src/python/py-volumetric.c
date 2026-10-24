/* SPDX-License-Identifier: Apache-2.0
 * Level sets and tetrahedral meshes (lightusd-import-c.h volumetric API).
 * Arrays cross as contiguous buffers in and native-endian bytes out; the
 * Python wrapper (lightusd.volumetric) adds shapes. */
#include "py-internal.h"

#include "lightusd-import-c.h"

static int get_buffer(PyObject* obj, Py_buffer* view, Py_ssize_t item, const char* what) {
  if (PyObject_GetBuffer(obj, view, PyBUF_CONTIG_RO) != 0) return 0;
  if (view->len % item) {
    PyBuffer_Release(view);
    PyErr_Format(PyExc_ValueError, "%s: buffer size is not a multiple of %zd bytes", what, item);
    return 0;
  }
  return 1;
}

static PyObject* take_string(lightusd_string* s) {
  lightusd_sv v = lightusd_string_view(s);
  PyObject* out = PyBytes_FromStringAndSize(v.data, (Py_ssize_t)v.len);
  lightusd_string_destroy(s);
  return out;
}

PyObject* lightusd_py_mesh_to_level_set(PyObject* module, PyObject* args) {
  PyObject *points_obj, *indices_obj, *counts_obj;
  const char* settings;
  if (!PyArg_ParseTuple(args, "OOOs:_mesh_to_level_set", &points_obj, &indices_obj, &counts_obj,
                        &settings))
    return NULL;
  lightusd_state* st = lightusd_state_from_module(module);
  Py_buffer p = {0}, i = {0}, c = {0};
  if (!get_buffer(points_obj, &p, 12, "points")) return NULL;
  if (!get_buffer(indices_obj, &i, 4, "indices")) {
    PyBuffer_Release(&p);
    return NULL;
  }
  if (counts_obj != Py_None && !get_buffer(counts_obj, &c, 4, "counts")) {
    PyBuffer_Release(&p);
    PyBuffer_Release(&i);
    return NULL;
  }
  lightusd_string* vdb = NULL;
  lightusd_status status;
  Py_BEGIN_ALLOW_THREADS status = lightusd_mesh_to_level_set_vdb(
      (const float*)p.buf, (size_t)p.len / 12, (const int32_t*)i.buf, (size_t)i.len / 4,
      counts_obj != Py_None ? (const int32_t*)c.buf : NULL,
      counts_obj != Py_None ? (size_t)c.len / 4 : 0, settings, &vdb);
  Py_END_ALLOW_THREADS PyBuffer_Release(&p);
  PyBuffer_Release(&i);
  if (counts_obj != Py_None) PyBuffer_Release(&c);
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, "mesh_to_level_set");
  return take_string(vdb);
}

PyObject* lightusd_py_level_set_info(PyObject* module, PyObject* args) {
  Py_buffer v = {0};
  if (!PyArg_ParseTuple(args, "y*:_level_set_info", &v)) return NULL;
  lightusd_state* st = lightusd_state_from_module(module);
  lightusd_level_set_info info;
  lightusd_status status = lightusd_level_set_vdb_info((const uint8_t*)v.buf, (size_t)v.len, &info);
  PyBuffer_Release(&v);
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, "level set");
  return Py_BuildValue("(dfK)", info.voxel_size, info.background,
                       (unsigned long long)info.active_voxels);
}

PyObject* lightusd_py_level_set_sample(PyObject* module, PyObject* args) {
  Py_buffer v = {0};
  PyObject* points_obj;
  if (!PyArg_ParseTuple(args, "y*O:_level_set_sample", &v, &points_obj)) return NULL;
  lightusd_state* st = lightusd_state_from_module(module);
  Py_buffer p = {0};
  if (!get_buffer(points_obj, &p, 24, "points")) {
    PyBuffer_Release(&v);
    return NULL;
  }
  const size_t count = (size_t)p.len / 24;
  PyObject* out = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)(count * sizeof(float)));
  if (!out) {
    PyBuffer_Release(&v);
    PyBuffer_Release(&p);
    return NULL;
  }
  float* values = (float*)PyBytes_AsString(out);
  lightusd_status status;
  Py_BEGIN_ALLOW_THREADS status = lightusd_level_set_vdb_sample(
      (const uint8_t*)v.buf, (size_t)v.len, (const double*)p.buf, count, values);
  Py_END_ALLOW_THREADS PyBuffer_Release(&v);
  PyBuffer_Release(&p);
  if (status != LIGHTUSD_OK) {
    Py_DECREF(out);
    return lightusd_raise(st, status, "level set sample");
  }
  return out;
}

PyObject* lightusd_py_level_set_to_mesh(PyObject* module, PyObject* args) {
  Py_buffer v = {0};
  unsigned long long max_cells = 0;
  if (!PyArg_ParseTuple(args, "y*K:_level_set_to_mesh", &v, &max_cells)) return NULL;
  lightusd_state* st = lightusd_state_from_module(module);
  lightusd_string *points = NULL, *indices = NULL;
  lightusd_status status;
  Py_BEGIN_ALLOW_THREADS status = lightusd_level_set_vdb_to_mesh(
      (const uint8_t*)v.buf, (size_t)v.len, max_cells, &points, &indices);
  Py_END_ALLOW_THREADS PyBuffer_Release(&v);
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, "level set mesh");
  PyObject* a = take_string(points);
  PyObject* b = take_string(indices);
  if (!a || !b) {
    Py_XDECREF(a);
    Py_XDECREF(b);
    return NULL;
  }
  return Py_BuildValue("(NN)", a, b);
}

PyObject* lightusd_py_level_set_to_tet_mesh(PyObject* module, PyObject* args) {
  Py_buffer v = {0};
  const char* settings;
  if (!PyArg_ParseTuple(args, "y*s:_level_set_to_tet_mesh", &v, &settings)) return NULL;
  lightusd_state* st = lightusd_state_from_module(module);
  lightusd_string *points = NULL, *tets = NULL, *surface = NULL;
  lightusd_status status;
  Py_BEGIN_ALLOW_THREADS status = lightusd_level_set_vdb_to_tet_mesh(
      (const uint8_t*)v.buf, (size_t)v.len, settings, &points, &tets, &surface);
  Py_END_ALLOW_THREADS PyBuffer_Release(&v);
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, "tet mesh");
  PyObject* a = take_string(points);
  PyObject* b = take_string(tets);
  PyObject* c = take_string(surface);
  if (!a || !b || !c) {
    Py_XDECREF(a);
    Py_XDECREF(b);
    Py_XDECREF(c);
    return NULL;
  }
  return Py_BuildValue("(NNN)", a, b, c);
}

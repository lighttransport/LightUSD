/* SPDX-License-Identifier: Apache-2.0 */
#include "lightusd-import-c.h"
#include "py-internal.h"

/* Consumes the native string; all bindings use the same JSON contract. */
PyObject* lightusd_import_report_object(lightusd_string* report) {
  PyObject* text = lightusd_sv_to_str(lightusd_string_view(report));
  lightusd_string_destroy(report);
  if (!text) return NULL;
  PyObject* json = PyImport_ImportModule("json");
  PyObject* result = json ? PyObject_CallMethod(json, "loads", "O", text) : NULL;
  Py_XDECREF(json);
  Py_DECREF(text);
  return result;
}
static void attach_import_report(void) {
  PyObject *type = NULL, *value = NULL, *traceback = NULL;
  PyErr_Fetch(&type, &value, &traceback);
  PyErr_NormalizeException(&type, &value, &traceback);
  lightusd_string* report = NULL;
  if (value && lightusd_import_last_report(&report) == LIGHTUSD_OK) {
    PyObject* object = lightusd_import_report_object(report);
    if (object) {
      if (PyObject_SetAttrString(value, "import_report", object) < 0) PyErr_Clear();
      Py_DECREF(object);
    }
  }
  PyErr_Clear();
  PyErr_Restore(type, value, traceback);
}

/* progress(phase, done, total) runs on the importing thread with the GIL
 * re-acquired. Returning False cancels; an exception cancels and is
 * re-raised after the import returns. */
typedef struct {
  PyObject* fn;
  PyObject *exc_type, *exc_value, *exc_tb;
} import_progress;

static int import_progress_call(void* user, const char* phase, uint64_t done,
                                uint64_t total) {
  import_progress* ctx = (import_progress*)user;
  if (ctx->exc_type) return 1;
  PyGILState_STATE gil = PyGILState_Ensure();
  PyObject* result = PyObject_CallFunction(ctx->fn, "sKK", phase,
                                           (unsigned long long)done,
                                           (unsigned long long)total);
  int cancel = 0;
  if (!result) {
    PyErr_Fetch(&ctx->exc_type, &ctx->exc_value, &ctx->exc_tb);
    cancel = 1;
  } else {
    cancel = result == Py_False;
    Py_DECREF(result);
  }
  PyGILState_Release(gil);
  return cancel;
}

PyObject* lightusd_import_scene(PyObject* module, PyObject* args,
                                PyObject* kwargs) {
  static char* names[] = {"path",     "data",       "source",   "format",
                          "settings", "max_memory", "resolver", "max_input", "max_depth",
                          "progress", NULL};
  const char *path = NULL, *source = "", *format = "auto", *settings = "{}";
  PyObject *data = Py_None, *resolver_obj = Py_None, *progress_obj = Py_None;
  PyObject *memory_obj = Py_None, *input_obj = Py_None, *depth_obj = Py_None;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|zOsssOOOOO:_import_scene",
                                   names, &path, &data, &source, &format,
                                   &settings, &memory_obj, &resolver_obj, &input_obj, &depth_obj,
                                   &progress_obj))
    return NULL;
  if (progress_obj != Py_None && !PyCallable_Check(progress_obj)) {
    PyErr_SetString(PyExc_TypeError, "progress must be callable");
    return NULL;
  }
  import_progress progress = {progress_obj, NULL, NULL, NULL};
  lightusd_state* st = lightusd_state_from_module(module);
  if (!st) return NULL;
  lightusd_import_options opts;
  lightusd_import_options_init(&opts);
  if (strcmp(format, "auto") == 0)
    opts.format = LIGHTUSD_IMPORT_AUTO;
  else if (strcmp(format, "obj") == 0)
    opts.format = LIGHTUSD_IMPORT_OBJ;
  else if (strcmp(format, "urdf") == 0)
    opts.format = LIGHTUSD_IMPORT_URDF;
  else if (strcmp(format, "mjcf") == 0)
    opts.format = LIGHTUSD_IMPORT_MJCF;
  else {
    PyErr_SetString(PyExc_ValueError,
                    "import format must be auto, obj, urdf or mjcf");
    return NULL;
  }
  opts.settings_json = settings;
  unsigned long long max_memory = 0;
  if (memory_obj != Py_None) {
    max_memory = PyLong_AsUnsignedLongLong(memory_obj);
    if (PyErr_Occurred()) return NULL;
  }
  if (max_memory) {
    opts.max_memory_bytes = max_memory;
    opts.max_input_bytes = max_memory;
  }
  if (input_obj != Py_None) {
    opts.max_input_bytes = PyLong_AsUnsignedLongLong(input_obj);
    if (PyErr_Occurred()) return NULL;
  }
  if (depth_obj != Py_None) {
    unsigned long long depth = PyLong_AsUnsignedLongLong(depth_obj);
    if (PyErr_Occurred()) return NULL;
    if (!depth || depth > 1024) {
      PyErr_SetString(PyExc_ValueError, "max_depth must be in [1, 1024]");
      return NULL;
    }
    opts.max_depth = (uint32_t)depth;
  }
  if (progress_obj != Py_None) {
    opts.progress = import_progress_call;
    opts.progress_user_data = &progress;
  }
  opts.resolver = lightusd_resolver_handle(st, resolver_obj);
  if (resolver_obj != Py_None && !opts.resolver) return NULL;
  if ((path != NULL) == (data != Py_None)) {
    PyErr_SetString(PyExc_ValueError, "provide exactly one of path or data");
    return NULL;
  }
  Py_buffer view = {0};
  if (data != Py_None && PyObject_GetBuffer(data, &view, PyBUF_CONTIG_RO) != 0)
    return NULL;
  lightusd_stage* stage = NULL;
  lightusd_status status;
  lightusd_resolver_clear_error(resolver_obj);
  Py_BEGIN_ALLOW_THREADS if (path) status =
      lightusd_import_file(path, &opts, &stage);
  else status = lightusd_import_memory((const uint8_t*)view.buf,
                                       (size_t)view.len, source, &opts, &stage);
  Py_END_ALLOW_THREADS if (data != Py_None) PyBuffer_Release(&view);
  if (progress.exc_type) {
    if (stage) lightusd_stage_destroy(stage);
    PyErr_Restore(progress.exc_type, progress.exc_value, progress.exc_tb);
    attach_import_report();
    return NULL;
  }
  if (lightusd_resolver_restore_error(resolver_obj)) {
    if (stage) lightusd_stage_destroy(stage);
    attach_import_report();
    return NULL;
  }
  if (status != LIGHTUSD_OK) {
    lightusd_raise(st, status, path ? path : "import");
    attach_import_report();
    return NULL;
  }
  return lightusd_wrap_stage(st, stage);
}

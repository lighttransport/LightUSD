/* SPDX-License-Identifier: Apache-2.0 */
#include "py-internal.h"
#include "lightusd-validation-c.h"

static int put(PyObject* dict, const char* name, PyObject* value) {
  if (!value) return -1;
  int status = PyDict_SetItemString(dict, name, value);
  Py_DECREF(value);
  return status;
}

PyObject* lightusd_validate_physics(PyObject* module, PyObject* args, PyObject* kwargs) {
  static char* names[] = {"stage", "include_core", "samples", "require_complete",
      "max_issues", "max_report_bytes", "max_samples", NULL};
  PyObject *object, *max_issues = NULL, *max_bytes = NULL, *max_samples = NULL;
  int include_core = 1, samples = 0, require_complete = 0;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|pppOOO:_validate_physics", names,
      &object, &include_core, &samples, &require_complete, &max_issues, &max_bytes, &max_samples)) return NULL;
  lightusd_state* st = lightusd_state_from_module(module);
  if (!st) return NULL;
  if (!PyObject_TypeCheck(object, (PyTypeObject*)st->StageType)) {
    PyErr_SetString(PyExc_TypeError, "expected a lightusd.Stage");
    return NULL;
  }
  lightusd_stage* stage = lightusd_stage_handle(object);
  if (!stage) { PyErr_SetString(st->UsdError, "Stage is closed"); return NULL; }
  lightusd_validation_options options;
  lightusd_validation_options_init(&options);
  options.groups = LIGHTUSD_VALIDATE_PHYSICS | (include_core ? LIGHTUSD_VALIDATE_CORE : 0u);
  if (samples) options.flags |= LIGHTUSD_VALIDATE_SAMPLES;
  if (require_complete) options.flags |= LIGHTUSD_VALIDATE_REQUIRE_COMPLETE;
  if (max_issues) options.max_issues = PyLong_AsUnsignedLongLong(max_issues);
  if (!PyErr_Occurred() && max_bytes) options.max_report_bytes = PyLong_AsUnsignedLongLong(max_bytes);
  if (!PyErr_Occurred() && max_samples) options.max_samples = PyLong_AsUnsignedLongLong(max_samples);
  if (PyErr_Occurred()) return NULL;
  lightusd_validation_report* report = NULL;
  lightusd_status status = lightusd_stage_validate(stage, &options, &report);
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, "validate_physics");
  lightusd_validation_summary summary;
  status = lightusd_validation_report_summary(report, &summary);
  if (status != LIGHTUSD_OK || summary.issue_count > (size_t)PY_SSIZE_T_MAX) {
    lightusd_validation_report_destroy(report);
    return lightusd_raise(st, status == LIGHTUSD_OK ? LIGHTUSD_ERR_OUT_OF_MEMORY : status, "validation summary");
  }
  PyObject *result = PyDict_New(), *issues = PyTuple_New((Py_ssize_t)summary.issue_count), *groups = PyList_New(0);
  int failed = !result || !issues || !groups;
  for (size_t i = 0; !failed && i < summary.issue_count; ++i) {
    lightusd_validation_issue issue;
    status = lightusd_validation_report_issue(report, i, &issue);
    if (status != LIGHTUSD_OK) { failed = 1; break; }
    PyObject* item = PyDict_New();
    if (!item) { failed = 1; break; }
    PyObject* time;
    if (issue.has_time) time = PyFloat_FromDouble(issue.time);
    else { time = Py_None; Py_INCREF(time); }
    if (!time || PyDict_SetItemString(item, "time", time) < 0) failed = 1;
    Py_XDECREF(time);
    if (!failed) failed = put(item, "severity", PyUnicode_FromString(issue.severity ? "warning" : "error")) < 0 ||
        put(item, "rule_id", lightusd_sv_to_str(issue.rule_id)) < 0 ||
        put(item, "location", lightusd_sv_to_str(issue.location)) < 0 ||
        put(item, "message", lightusd_sv_to_str(issue.message)) < 0 ||
        put(item, "source_asset", lightusd_sv_to_str(issue.source_asset)) < 0 ||
        put(item, "variants", lightusd_sv_to_str(issue.variants)) < 0;
    if (failed) Py_DECREF(item);
    else PyTuple_SetItem(issues, (Py_ssize_t)i, item);
  }
  const struct { uint32_t bit; const char* name; } group_names[] = {
      {LIGHTUSD_VALIDATE_CORE, "core"}, {LIGHTUSD_VALIDATE_GEOM, "geom"},
      {LIGHTUSD_VALIDATE_SHADE, "shade"}, {LIGHTUSD_VALIDATE_LUX, "lux"},
      {LIGHTUSD_VALIDATE_PHYSICS, "physics"}, {LIGHTUSD_VALIDATE_RENDER, "render"},
      {LIGHTUSD_VALIDATE_ARKIT, "arkit"}};
  for (size_t i = 0; !failed && i < sizeof(group_names) / sizeof(group_names[0]); ++i)
    if (summary.checked_groups & group_names[i].bit) {
      PyObject* name = PyUnicode_FromString(group_names[i].name);
      if (!name || PyList_Append(groups, name) < 0) failed = 1;
      Py_XDECREF(name);
    }
  if (!failed) failed = PyDict_SetItemString(result, "issues", issues) < 0 ||
      PyDict_SetItemString(result, "checked_groups", groups) < 0 ||
      put(result, "error_count", PyLong_FromSize_t(summary.error_count)) < 0 ||
      put(result, "warning_count", PyLong_FromSize_t(summary.warning_count)) < 0 ||
      put(result, "omitted_issues", PyLong_FromSize_t(summary.omitted_issues)) < 0 ||
      put(result, "complete", PyBool_FromLong(summary.complete)) < 0 ||
      put(result, "valid", PyBool_FromLong(summary.valid)) < 0;
  Py_XDECREF(issues); Py_XDECREF(groups);
  lightusd_validation_report_destroy(report);
  if (failed) {
    Py_XDECREF(result);
    if (!PyErr_Occurred()) lightusd_raise(st, status == LIGHTUSD_OK ? LIGHTUSD_ERR_INTERNAL : status, "validation report");
    return NULL;
  }
  return result;
}

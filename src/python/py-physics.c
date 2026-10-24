/* SPDX-License-Identifier: Apache-2.0 */
#include "lightusd-render-c.h"
#include "lightusd-physics-c.h"
#include "next/c/lightusd-core-c.h"
#include "py-internal.h"

static int put(PyObject* dict, const char* key, PyObject* value) {
  if (!value) return -1;
  int result = PyDict_SetItemString(dict, key, value);
  Py_DECREF(value);
  return result;
}
static PyObject* vector3(const float* v) {
  return Py_BuildValue("(fff)", v[0], v[1], v[2]);
}
static PyObject* schema_text(lu_view text) {
  lightusd_sv view = {text.data, text.len};
  return lightusd_sv_to_str(view);
}

PyObject* lightusd_physics_schema_definitions(PyObject* module, PyObject* noargs) {
  (void)module; (void)noargs;
  size_t count = 0;
  if (lu_validation_schema_count(&count) != LU_OK) {
    PyErr_SetString(PyExc_RuntimeError, "Cannot enumerate native schema descriptors");
    return NULL;
  }
  PyObject* result = PyDict_New();
  for (size_t i = 0; result && i < count; ++i) {
    lu_validation_schema_view schema;
    if (lu_validation_schema_at(i, &schema) != LU_OK) { Py_CLEAR(result); break; }
    /* Standard UsdPhysics plus the versioned MuJoCo/Newton engine schemas. */
    const int physics = schema.name.len >= 7 && memcmp(schema.name.data, "Physics", 7) == 0;
    const int mjc = schema.name.len >= 3 && memcmp(schema.name.data, "Mjc", 3) == 0;
    const int newton = schema.name.len >= 6 && memcmp(schema.name.data, "Newton", 6) == 0;
    if (!physics && !mjc && !newton) continue;
    PyObject *key = schema_text(schema.name), *record = PyDict_New(), *properties = PyDict_New();
    int failed = !key || !record || !properties;
    if (!failed) failed = put(record, "parent", schema_text(schema.parent)) < 0 ||
        put(record, "kind", schema_text(schema.kind)) < 0 ||
        put(record, "property_namespace", schema_text(schema.property_namespace)) < 0 ||
        put(record, "source", schema_text(schema.source)) < 0;
    for (size_t p = 0; !failed && p < schema.property_count; ++p) {
      lu_validation_property_view property;
      if (lu_validation_property_at(i, p, &property) != LU_OK || property.token_count > (size_t)PY_SSIZE_T_MAX) { failed = 1; break; }
      PyObject *name = schema_text(property.name), *item = PyDict_New(), *tokens = PyTuple_New((Py_ssize_t)property.token_count);
      if (!name || !item || !tokens) failed = 1;
      for (size_t t = 0; !failed && t < property.token_count; ++t) {
        lu_view token;
        if (lu_validation_property_token(i, p, t, &token) != LU_OK) { failed = 1; break; }
        PyObject* text = schema_text(token);
        if (!text) failed = 1;
        else PyTuple_SetItem(tokens, (Py_ssize_t)t, text);
      }
      if (!failed) failed = put(item, "type", schema_text(property.type)) < 0 ||
          put(item, "variability", schema_text(property.variability)) < 0 ||
          put(item, "relationship", PyBool_FromLong(property.is_relationship)) < 0 ||
          PyDict_SetItemString(item, "allowed_tokens", tokens) < 0 ||
          PyDict_SetItem(properties, name, item) < 0;
      Py_XDECREF(name); Py_XDECREF(item); Py_XDECREF(tokens);
    }
    if (!failed) failed = PyDict_SetItemString(record, "properties", properties) < 0 || PyDict_SetItem(result, key, record) < 0;
    Py_XDECREF(key); Py_XDECREF(record); Py_XDECREF(properties);
    if (failed) Py_CLEAR(result);
  }
  if (!result && !PyErr_Occurred()) PyErr_SetString(PyExc_RuntimeError, "Invalid native schema descriptors");
  return result;
}

static PyObject* physics_targets(lightusd_strlist* targets) {
  size_t count = lightusd_strlist_size(targets);
  if (count > (size_t)PY_SSIZE_T_MAX) { lightusd_strlist_destroy(targets); return PyErr_NoMemory(); }
  PyObject* result = PyTuple_New((Py_ssize_t)count);
  for (size_t i = 0; result && i < count; ++i) {
    PyObject* text = lightusd_sv_to_str(lightusd_strlist_get(targets, i));
    if (!text) { Py_CLEAR(result); break; }
    PyTuple_SetItem(result, (Py_ssize_t)i, text);
  }
  lightusd_strlist_destroy(targets);
  return result;
}

PyObject* lightusd_query_physics(PyObject* module, PyObject* args, PyObject* kwargs) {
  static char* names[] = {"stage", "time", "max_memory", "max_records", NULL};
  PyObject *stage_obj, *time = NULL, *memory = NULL, *records = NULL;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|OOO:_query_physics", names,
                                   &stage_obj, &time, &memory, &records)) return NULL;
  lightusd_state* st = lightusd_state_from_module(module);
  if (!st) return NULL;
  if (!PyObject_TypeCheck(stage_obj, (PyTypeObject*)st->StageType)) {
    PyErr_SetString(PyExc_TypeError, "expected a lightusd.Stage");
    return NULL;
  }
  lightusd_stage* stage = lightusd_stage_handle(stage_obj);
  if (!stage) { PyErr_SetString(st->UsdError, "Stage is closed"); return NULL; }
  lightusd_physics_query_options options;
  lightusd_physics_query_options_init(&options);
  if (time == Py_None) options.flags = LIGHTUSD_PHYSICS_DEFAULT_TIME;
  else if (time) { options.time_code = PyFloat_AsDouble(time); if (PyErr_Occurred()) return NULL; }
  if (memory) {
    unsigned long long size = PyLong_AsUnsignedLongLong(memory);
    if (PyErr_Occurred()) return NULL;
    if (size) options.max_memory_bytes = size;
  }
  if (records) {
    options.max_records = PyLong_AsUnsignedLongLong(records);
    if (PyErr_Occurred()) return NULL;
  }
  lightusd_physics_snapshot* snapshot = NULL;
  lightusd_status status = lightusd_stage_query_physics(stage, &options, &snapshot);
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, "query_physics");
  const size_t count = lightusd_physics_snapshot_size(snapshot);
  PyObject* result = count <= (size_t)PY_SSIZE_T_MAX ? PyTuple_New((Py_ssize_t)count) : PyErr_NoMemory();
  for (size_t i = 0; result && i < count; ++i) {
    lightusd_physics_record_info info;
    status = lightusd_physics_snapshot_record(snapshot, i, &info);
    if (status != LIGHTUSD_OK || info.schema_count > (size_t)PY_SSIZE_T_MAX) {
      Py_CLEAR(result);
      if (!PyErr_Occurred()) lightusd_raise(st, status == LIGHTUSD_OK ? LIGHTUSD_ERR_OUT_OF_MEMORY : status, "physics record");
      break;
    }
    PyObject *record = PyDict_New(), *schemas = PyTuple_New((Py_ssize_t)info.schema_count),
             *attributes = PyDict_New(), *types = PyDict_New(), *relationships = PyDict_New(),
             *uniform = PyList_New(0), *blocked = PyList_New(0), *groups = PyList_New(0);
    int failed = !record || !schemas || !attributes || !types || !relationships || !uniform || !blocked || !groups;
    for (size_t k = 0; !failed && k < info.schema_count; ++k) {
      lightusd_sv name;
      status = lightusd_physics_snapshot_schema(snapshot, i, k, &name);
      PyObject* text = status == LIGHTUSD_OK ? lightusd_sv_to_str(name) : NULL;
      if (!text) failed = 1;
      else PyTuple_SetItem(schemas, (Py_ssize_t)k, text);
    }
    for (size_t k = 0; !failed && k < info.property_count; ++k) {
      lightusd_physics_property_info property;
      lightusd_value* owned = NULL;
      status = lightusd_physics_snapshot_property(snapshot, i, k, &property, &owned);
      if (status != LIGHTUSD_OK) { failed = 1; break; }
      lightusd_value_view view;
      status = lightusd_value_get_view(owned, &view);
      PyObject *name = lightusd_sv_to_str(property.name), *type = lightusd_sv_to_str(property.type_name), *value = NULL;
      if (status == LIGHTUSD_OK && view.type == LIGHTUSD_TYPE_INVALID && !view.is_block) {
        Py_INCREF(Py_None); value = Py_None; lightusd_value_destroy(owned);
      } else if (status == LIGHTUSD_OK) value = lightusd_value_to_python(st, owned);
      else lightusd_value_destroy(owned);
      if (!name || !type || !value || PyDict_SetItem(attributes, name, value) < 0 ||
          PyDict_SetItem(types, name, type) < 0 || (property.uniform && PyList_Append(uniform, name) < 0) ||
          (property.blocked && PyList_Append(blocked, name) < 0)) failed = 1;
      Py_XDECREF(name); Py_XDECREF(type); Py_XDECREF(value);
    }
    for (size_t k = 0; !failed && k < info.relationship_count; ++k) {
      lightusd_sv name;
      lightusd_strlist* targets = NULL;
      status = lightusd_physics_snapshot_relationship(snapshot, i, k, &name, &targets);
      if (status != LIGHTUSD_OK) { failed = 1; break; }
      PyObject *key = lightusd_sv_to_str(name), *value = physics_targets(targets);
      if (!key || !value || PyDict_SetItem(relationships, key, value) < 0) failed = 1;
      Py_XDECREF(key); Py_XDECREF(value);
    }
    for (uint32_t k = 0; !failed && k < LIGHTUSD_PHYSICS_GROUP_COUNT; ++k)
      if (info.groups & (UINT64_C(1) << k)) {
        PyObject* name = PyUnicode_FromString(lightusd_physics_group_name(k));
        if (!name || PyList_Append(groups, name) < 0) failed = 1;
        Py_XDECREF(name);
      }
    if (!failed) failed = put(record, "prim_path", lightusd_sv_to_str(info.prim_path)) < 0 ||
        put(record, "type_name", lightusd_sv_to_str(info.type_name)) < 0 ||
        PyDict_SetItemString(record, "api_schemas", schemas) < 0 ||
        PyDict_SetItemString(record, "attributes", attributes) < 0 ||
        PyDict_SetItemString(record, "property_types", types) < 0 ||
        PyDict_SetItemString(record, "relationships", relationships) < 0 ||
        PyDict_SetItemString(record, "uniform_properties", uniform) < 0 ||
        PyDict_SetItemString(record, "blocked_properties", blocked) < 0 ||
        PyDict_SetItemString(record, "groups", groups) < 0;
    Py_XDECREF(schemas); Py_XDECREF(attributes); Py_XDECREF(types); Py_XDECREF(relationships);
    Py_XDECREF(uniform); Py_XDECREF(blocked); Py_XDECREF(groups);
    if (failed) {
      Py_XDECREF(record); Py_CLEAR(result);
      if (!PyErr_Occurred()) lightusd_raise(st, status == LIGHTUSD_OK ? LIGHTUSD_ERR_INTERNAL : status, "physics snapshot");
    } else PyTuple_SetItem(result, (Py_ssize_t)i, record);
  }
  lightusd_physics_snapshot_destroy(snapshot);
  return result;
}

PyObject* lightusd_physics_annotations(lightusd_state* st,
                                       lightusd_render_scene* scene) {
  const char* names[] = {
      "scenes",    "rigid_bodies",   "colliders",         "joints",
      "materials", "filtered_pairs", "articulation_roots"};
  PyObject* result = PyDict_New();
  if (!result) return NULL;
  for (uint8_t kind = 0; kind < 7; kind++) {
    size_t count = lightusd_render_physics_count(scene, kind);
    if (count > (size_t)PY_SSIZE_T_MAX) {
      Py_DECREF(result);
      return PyErr_NoMemory();
    }
    PyObject* items = PyTuple_New((Py_ssize_t)count);
    if (!items) {
      Py_DECREF(result);
      return NULL;
    }
    for (size_t i = 0; i < count; i++) {
      lightusd_render_physics_info info;
      lightusd_status status =
          lightusd_render_physics_get_info(scene, kind, i, &info);
      if (status != LIGHTUSD_OK) {
        Py_DECREF(items);
        Py_DECREF(result);
        return lightusd_raise(st, status, "physics record");
      }
      PyObject* record = PyDict_New();
      if (!record) {
        Py_DECREF(items);
        Py_DECREF(result);
        return NULL;
      }
      int failed =
          put(record, "prim_path", lightusd_sv_to_str(info.prim_path)) < 0 ||
          put(record, "type_name", lightusd_sv_to_str(info.type_name)) < 0;
      if (kind == 0)
        failed |= put(record, "gravity_direction", vector3(info.vector0)) < 0 ||
                  put(record, "gravity_magnitude",
                      PyFloat_FromDouble(info.scalar0)) < 0;
      if (kind == 1) {
        failed |= put(record, "velocity", vector3(info.vector0)) < 0 ||
                  put(record, "angular_velocity", vector3(info.vector1)) < 0 ||
                  put(record, "center_of_mass", vector3(info.vector2)) < 0;
        failed |= put(record, "mass", PyFloat_FromDouble(info.scalar0)) < 0 ||
                  put(record, "density", PyFloat_FromDouble(info.scalar1)) < 0;
        failed |=
            put(record, "rigid_body_enabled", PyBool_FromLong(info.flags & 1)) <
                0 ||
            put(record, "kinematic_enabled", PyBool_FromLong(info.flags & 2)) <
                0 ||
            put(record, "starts_asleep", PyBool_FromLong(info.flags & 4)) < 0;
      }
      if (kind == 2)
        failed |= put(record, "collision_enabled",
                      PyBool_FromLong(info.flags & 1)) < 0 ||
                  put(record, "approximation",
                      lightusd_sv_to_str(info.approximation)) < 0;
      if (kind == 3) {
        failed |= put(record, "body0", lightusd_sv_to_str(info.body0)) < 0 ||
                  put(record, "body1", lightusd_sv_to_str(info.body1)) < 0;
        failed |= put(record, "local_pos0", vector3(info.vector0)) < 0 ||
                  put(record, "local_pos1", vector3(info.vector1)) < 0 ||
                  put(record, "axis", vector3(info.vector2)) < 0;
        failed |=
            put(record, "lower_limit", PyFloat_FromDouble(info.scalar0)) < 0 ||
            put(record, "upper_limit", PyFloat_FromDouble(info.scalar1)) < 0;
      }
      if (kind == 4)
        failed |=
            put(record, "static_friction", PyFloat_FromDouble(info.scalar0)) <
                0 ||
            put(record, "dynamic_friction", PyFloat_FromDouble(info.scalar1)) <
                0 ||
            put(record, "restitution", PyFloat_FromDouble(info.scalar2)) < 0 ||
            put(record, "density", PyFloat_FromDouble(info.scalar3)) < 0;
      if (kind == LIGHTUSD_RENDER_PHYSICS_FILTERED_PAIR) {
        PyObject* targets = PyList_New(0);
        if (!targets) failed = 1;
        for (size_t target = 0; !failed; target++) {
          size_t required = 0;
          status = lightusd_render_physics_string_copy(
              scene, kind, i, 0, target, NULL, 0, &required);
          if (status == LIGHTUSD_ERR_NOT_FOUND) break;
          if (status != LIGHTUSD_OK || required > (size_t)PY_SSIZE_T_MAX) {
            failed = 1;
            break;
          }
          char* buffer = (char*)PyMem_Malloc(required ? required : 1);
          if (!buffer) {
            PyErr_NoMemory();
            failed = 1;
            break;
          }
          status = lightusd_render_physics_string_copy(
              scene, kind, i, 0, target, buffer, required, &required);
          PyObject* path =
              status == LIGHTUSD_OK
                  ? PyUnicode_DecodeUTF8(buffer, (Py_ssize_t)required, "strict")
                  : NULL;
          PyMem_Free(buffer);
          if (!path || PyList_Append(targets, path) < 0) failed = 1;
          Py_XDECREF(path);
        }
        if (targets) {
          PyObject* tuple = PyList_AsTuple(targets);
          Py_DECREF(targets);
          if (put(record, "targets", tuple) < 0) failed = 1;
        }
      }
      PyObject* extensions = PyDict_New();
      if (!extensions) failed = 1;
      for (size_t p = 0; !failed; p++) {
        lightusd_sv name, value;
        status = lightusd_render_physics_extension_property(scene, kind, i, p,
                                                            &name, &value);
        if (status == LIGHTUSD_ERR_NOT_FOUND) break;
        if (status != LIGHTUSD_OK) {
          failed = 1;
          break;
        }
        PyObject *key = lightusd_sv_to_str(name),
                 *text = lightusd_sv_to_str(value);
        if (!key || !text || PyDict_SetItem(extensions, key, text) < 0)
          failed = 1;
        Py_XDECREF(key);
        Py_XDECREF(text);
      }
      if (extensions)
        failed |= put(record, "extension_summaries", extensions) < 0;
      if (failed) {
        Py_DECREF(record);
        Py_DECREF(items);
        Py_DECREF(result);
        if (!PyErr_Occurred())
          lightusd_raise(st, LIGHTUSD_ERR_INTERNAL, "physics annotations");
        return NULL;
      }
      PyTuple_SetItem(items, (Py_ssize_t)i, record);
    }
    if (put(result, names[kind], items) < 0) {
      Py_DECREF(result);
      return NULL;
    }
  }
  return result;
}

PyObject* lightusd_convert_units(PyObject* module, PyObject* args, PyObject* kwargs) {
  static char* names[] = {"stage", "meters_per_unit", "kilograms_per_unit", "up_axis", NULL};
  PyObject* stage_obj;
  double mpu = 0, kpu = 0;
  const char* up_axis = NULL;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|ddz:_convert_units", names,
                                   &stage_obj, &mpu, &kpu, &up_axis)) return NULL;
  lightusd_state* st = lightusd_state_from_module(module);
  if (!st) return NULL;
  if (!PyObject_TypeCheck(stage_obj, (PyTypeObject*)st->StageType)) {
    PyErr_SetString(PyExc_TypeError, "expected a lightusd.Stage");
    return NULL;
  }
  lightusd_stage* stage = lightusd_stage_handle(stage_obj);
  if (!stage) { PyErr_SetString(st->UsdError, "Stage is closed"); return NULL; }
  lightusd_unit_conversion report;
  lightusd_strlist* skipped = NULL;
  lightusd_status status;
  Py_BEGIN_ALLOW_THREADS
  status = lightusd_stage_convert_units(stage, mpu, kpu, up_axis, &report, &skipped);
  Py_END_ALLOW_THREADS
  if (status == LIGHTUSD_ERR_INVALID_ARG) {
    PyErr_SetString(PyExc_ValueError, lightusd_last_error());
    return NULL;
  }
  if (status != LIGHTUSD_OK) return lightusd_raise(st, status, "convert_units");
  const size_t count = lightusd_strlist_size(skipped);
  PyObject* names_out = PyList_New(0);
  for (size_t i = 0; names_out && i < count; ++i) {
    PyObject* name = lightusd_sv_to_str(lightusd_strlist_get(skipped, i));
    if (!name || PyList_Append(names_out, name) < 0) Py_CLEAR(names_out);
    Py_XDECREF(name);
  }
  lightusd_strlist_destroy(skipped);
  if (!names_out) return NULL;
  return Py_BuildValue("(ddnnN)", report.length_scale, report.mass_scale,
                       (Py_ssize_t)report.converted, (Py_ssize_t)report.rotated_roots, names_out);
}


/* SPDX-License-Identifier: Apache 2.0
 *
 * Extended LightUSD Mesh API for ABI3 binding
 *
 * This provides additional functions for accessing GeomMesh data
 * such as points, indices, normals, and primvars.
 */

#define Py_LIMITED_API 0x030a0000
#include "../include/py_limited_api.h"
#include "../../../src/legacy/c-lightusd.h"

#include <string.h>

/* ============================================================================
 * Mesh Data Extraction
 * ============================================================================ */

/*
 * Get mesh points attribute as ValueArray with buffer protocol support
 *
 * This function demonstrates how to extract geometry data from a Prim
 * and wrap it in a ValueArray object for zero-copy NumPy access.
 *
 * In a complete implementation, this would:
 * 1. Get the Prim from the path
 * 2. Check if it's a Mesh prim
 * 3. Get the "points" attribute
 * 4. Get the array data
 * 5. Wrap it in ValueArray
 * 6. Return to Python for NumPy conversion
 */

PyObject *
LightUSDPrim_get_points(PyObject *self, PyObject *args)
{
    /* Placeholder implementation
     *
     * Full implementation would:
     * 1. Extract prim from self
     * 2. Check prim type is Mesh
     * 3. Get points attribute
     * 4. Create ValueArray wrapper
     * 5. Return ValueArray
     *
     * Example:
     *   LightUSDPrimObject *prim_obj = (LightUSDPrimObject *)self;
     *   CLightUSDProperty prop;
     *   if (!c_lightusd_prim_property_get(prim_obj->prim, "points", &prop)) {
     *       PyErr_SetString(PyExc_AttributeError, "Mesh has no 'points' attribute");
     *       return NULL;
     *   }
     *
     *   // Extract array data and wrap in ValueArray...
     */

    PyErr_SetString(PyExc_NotImplementedError,
                    "Mesh data extraction not yet implemented in C binding. "
                    "See example_mesh_to_numpy.py for API demonstration.");
    return NULL;
}

PyObject *
LightUSDPrim_get_face_vertex_indices(PyObject *self, PyObject *args)
{
    /* Placeholder - would extract faceVertexIndices attribute */
    PyErr_SetString(PyExc_NotImplementedError,
                    "faceVertexIndices extraction not yet implemented");
    return NULL;
}

PyObject *
LightUSDPrim_get_face_vertex_counts(PyObject *self, PyObject *args)
{
    /* Placeholder - would extract faceVertexCounts attribute */
    PyErr_SetString(PyExc_NotImplementedError,
                    "faceVertexCounts extraction not yet implemented");
    return NULL;
}

PyObject *
LightUSDPrim_get_normals(PyObject *self, PyObject *args)
{
    /* Placeholder - would extract normals primvar */
    PyErr_SetString(PyExc_NotImplementedError,
                    "Normals extraction not yet implemented");
    return NULL;
}

PyObject *
LightUSDPrim_get_primvar(PyObject *self, PyObject *args)
{
    const char *primvar_name;

    if (!PyArg_ParseTuple(args, "s", &primvar_name)) {
        return NULL;
    }

    /* Placeholder - would extract named primvar */
    PyErr_Format(PyExc_NotImplementedError,
                 "Primvar '%s' extraction not yet implemented",
                 primvar_name);
    return NULL;
}

/* ============================================================================
 * Stage Traversal Helpers
 * ============================================================================ */

PyObject *
LightUSDStage_get_prim_at_path(PyObject *self, PyObject *args)
{
    const char *path;

    if (!PyArg_ParseTuple(args, "s", &path)) {
        return NULL;
    }

    /* Placeholder - would traverse stage and find prim at path */
    PyErr_Format(PyExc_NotImplementedError,
                 "Stage traversal not yet implemented. Cannot get prim at path '%s'",
                 path);
    return NULL;
}

PyObject *
LightUSDStage_traverse_prims(PyObject *self, PyObject *args)
{
    /* Placeholder - would return iterator or list of all prims */
    PyErr_SetString(PyExc_NotImplementedError,
                    "Stage prim traversal not yet implemented");
    return NULL;
}

/* ============================================================================
 * TODO: These functions need to be added to the PyMethodDef tables
 * in lightusd_abi3.c
 *
 * Example:
 *
 * static PyMethodDef LightUSDPrim_methods[] = {
 *     // ... existing methods ...
 *     {"get_points", LightUSDPrim_get_points, METH_NOARGS,
 *      "Get mesh points as ValueArray"},
 *     {"get_face_vertex_indices", LightUSDPrim_get_face_vertex_indices, METH_NOARGS,
 *      "Get face vertex indices as ValueArray"},
 *     {"get_normals", LightUSDPrim_get_normals, METH_NOARGS,
 *      "Get normals as ValueArray"},
 *     {"get_primvar", LightUSDPrim_get_primvar, METH_VARARGS,
 *      "Get primvar by name as ValueArray"},
 *     {NULL}
 * };
 *
 * static PyMethodDef LightUSDStage_methods[] = {
 *     // ... existing methods ...
 *     {"get_prim_at_path", LightUSDStage_get_prim_at_path, METH_VARARGS,
 *      "Get prim at specified path"},
 *     {"traverse_prims", LightUSDStage_traverse_prims, METH_NOARGS,
 *      "Traverse all prims in stage"},
 *     {NULL}
 * };
 *
 * ============================================================================ */

/*
 * Implementation notes:
 *
 * To complete these functions, you need to:
 *
 * 1. Use the C API to access prim properties:
 *    - c_lightusd_prim_property_get()
 *    - c_lightusd_prim_get_property_names()
 *
 * 2. Extract array data from properties
 *    - Check property type
 *    - Get array length and data pointer
 *
 * 3. Create ValueArray wrapper:
 *    - Allocate LightUSDValueArrayObject
 *    - Set data pointer (from C++ vector)
 *    - Set length, itemsize, format
 *    - Set owner reference (to keep Prim alive)
 *    - Return to Python
 *
 * 4. NumPy will use buffer protocol to access the data zero-copy
 *
 * Example implementation sketch:
 *
 * PyObject * LightUSDPrim_get_points(PyObject *self, PyObject *args)
 * {
 *     LightUSDPrimObject *prim_obj = (LightUSDPrimObject *)self;
 *
 *     // Get points property
 *     CLightUSDProperty prop;
 *     if (!c_lightusd_prim_property_get(prim_obj->prim, "points", &prop)) {
 *         PyErr_SetString(PyExc_AttributeError, "No 'points' attribute");
 *         return NULL;
 *     }
 *
 *     // Check it's an attribute (not relationship)
 *     if (!c_lightusd_property_is_attribute(&prop)) {
 *         PyErr_SetString(PyExc_TypeError, "'points' is not an attribute");
 *         return NULL;
 *     }
 *
 *     // Get attribute value (this needs new C API function)
 *     CLightUSDValue *value = c_lightusd_attribute_get_value(&prop);
 *     if (!value) {
 *         PyErr_SetString(PyExc_RuntimeError, "Failed to get attribute value");
 *         return NULL;
 *     }
 *
 *     // Check it's an array type
 *     CLightUSDValueType vtype = c_lightusd_value_type(value);
 *     if (!(vtype & C_LIGHTUSD_VALUE_1D_BIT)) {
 *         PyErr_SetString(PyExc_TypeError, "'points' is not an array");
 *         return NULL;
 *     }
 *
 *     // Get array info (this needs new C API function)
 *     uint64_t length;
 *     void *data_ptr;
 *     if (!c_lightusd_value_get_array_info(value, &length, &data_ptr)) {
 *         PyErr_SetString(PyExc_RuntimeError, "Failed to get array info");
 *         return NULL;
 *     }
 *
 *     // Create ValueArray wrapper
 *     LightUSDValueArrayObject *array = PyObject_New(LightUSDValueArrayObject,
 *                                                     &LightUSDValueArrayType);
 *     if (!array) {
 *         return NULL;
 *     }
 *
 *     array->data = data_ptr;
 *     array->length = length;
 *     array->itemsize = c_lightusd_value_type_sizeof(vtype & ~C_LIGHTUSD_VALUE_1D_BIT);
 *     array->readonly = 1;  // USD data is typically read-only
 *     array->value_type = vtype & ~C_LIGHTUSD_VALUE_1D_BIT;
 *     array->format = NULL;  // Will be computed in getbuffer
 *     array->owner = (PyObject *)prim_obj;  // Keep prim alive
 *     Py_INCREF(array->owner);
 *
 *     return (PyObject *)array;
 * }
 */

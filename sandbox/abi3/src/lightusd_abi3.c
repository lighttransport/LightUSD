/* SPDX-License-Identifier: Apache 2.0
 *
 * LightUSD Python ABI3 Binding
 *
 * This module provides Python bindings for LightUSD using the Python 3.10+
 * stable ABI (Limited API). It supports numpy-friendly buffer protocol for
 * efficient array data access without copying.
 *
 * Key design principles:
 * 1. C++ side: RAII memory management (handled by c-lightusd.h)
 * 2. Python side: Reference counting for object lifetime
 * 3. Buffer protocol: Zero-copy array access for numpy compatibility
 */

#define Py_LIMITED_API 0x030a0000
#include "../include/py_limited_api.h"
#include "../../../src/legacy/c-lightusd.h"

#include <string.h>

/* Forward declarations */
static PyTypeObject LightUSDStageType;
static PyTypeObject LightUSDPrimType;
static PyTypeObject LightUSDValueType;
static PyTypeObject LightUSDValueArrayType;

/* ============================================================================
 * Stage Object
 * ============================================================================ */

typedef struct {
    PyObject_HEAD
    CLightUSDStage *stage;  /* Managed by RAII on C++ side */
} LightUSDStageObject;

static void
LightUSDStage_dealloc(LightUSDStageObject *self)
{
    if (self->stage) {
        c_lightusd_stage_free(self->stage);
        self->stage = NULL;
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
LightUSDStage_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    LightUSDStageObject *self;
    self = (LightUSDStageObject *)type->tp_alloc(type, 0);
    if (self != NULL) {
        self->stage = c_lightusd_stage_new();
        if (self->stage == NULL) {
            Py_DECREF(self);
            PyErr_SetString(PyExc_MemoryError, "Failed to create stage");
            return NULL;
        }
    }
    return (PyObject *)self;
}

static PyObject *
LightUSDStage_to_string(LightUSDStageObject *self, PyObject *Py_UNUSED(ignored))
{
    c_lightusd_string_t *str = c_lightusd_string_new_empty();
    if (!str) {
        return PyErr_NoMemory();
    }

    if (!c_lightusd_stage_to_string(self->stage, str)) {
        c_lightusd_string_free(str);
        PyErr_SetString(PyExc_RuntimeError, "Failed to convert stage to string");
        return NULL;
    }

    const char *cstr = c_lightusd_string_str(str);
    PyObject *result = PyUnicode_FromString(cstr);
    c_lightusd_string_free(str);
    return result;
}

static PyObject *
LightUSDStage_load_from_file(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    const char *filename;
    static char *kwlist[] = {"filename", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s", kwlist, &filename)) {
        return NULL;
    }

    LightUSDStageObject *self = (LightUSDStageObject *)LightUSDStage_new(type, NULL, NULL);
    if (self == NULL) {
        return NULL;
    }

    c_lightusd_string_t *warn = c_lightusd_string_new_empty();
    c_lightusd_string_t *err = c_lightusd_string_new_empty();

    int ret = c_lightusd_load_usd_from_file(filename, self->stage, warn, err);

    if (!ret) {
        const char *err_str = c_lightusd_string_str(err);
        PyErr_SetString(PyExc_RuntimeError, err_str);
        c_lightusd_string_free(warn);
        c_lightusd_string_free(err);
        Py_DECREF(self);
        return NULL;
    }

    /* TODO: Handle warnings */
    c_lightusd_string_free(warn);
    c_lightusd_string_free(err);

    return (PyObject *)self;
}

static PyMethodDef LightUSDStage_methods[] = {
    {"to_string", (PyCFunction)LightUSDStage_to_string, METH_NOARGS,
     "Convert stage to string representation"},
    {"load_from_file", (PyCFunction)LightUSDStage_load_from_file,
     METH_VARARGS | METH_KEYWORDS | METH_CLASS,
     "Load USD file into a new stage"},
    {NULL}
};

static PyTypeObject LightUSDStageType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "lightusd_abi3.Stage",
    .tp_doc = "LightUSD Stage object",
    .tp_basicsize = sizeof(LightUSDStageObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = LightUSDStage_new,
    .tp_dealloc = (destructor)LightUSDStage_dealloc,
    .tp_methods = LightUSDStage_methods,
};

/* ============================================================================
 * Value Array Object with Buffer Protocol
 * ============================================================================ */

typedef struct {
    PyObject_HEAD
    void *data;           /* Pointer to array data */
    Py_ssize_t length;    /* Number of elements */
    Py_ssize_t itemsize;  /* Size of each element in bytes */
    int readonly;         /* Is the buffer readonly? */
    char *format;         /* Format string for buffer protocol */
    CLightUSDValueType value_type;  /* LightUSD value type */
    PyObject *owner;      /* Owner object to keep alive */
} LightUSDValueArrayObject;

static void
LightUSDValueArray_dealloc(LightUSDValueArrayObject *self)
{
    Py_XDECREF(self->owner);
    if (self->format) {
        PyMem_Free(self->format);
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

/* Get format string for buffer protocol based on value type */
static const char *
get_format_string(CLightUSDValueType value_type)
{
    switch (value_type) {
        case C_LIGHTUSD_VALUE_BOOL: return "?";
        case C_LIGHTUSD_VALUE_INT: return "i";
        case C_LIGHTUSD_VALUE_UINT: return "I";
        case C_LIGHTUSD_VALUE_INT64: return "q";
        case C_LIGHTUSD_VALUE_UINT64: return "Q";
        case C_LIGHTUSD_VALUE_FLOAT: return "f";
        case C_LIGHTUSD_VALUE_DOUBLE: return "d";
        case C_LIGHTUSD_VALUE_HALF: return "e";  /* half-precision float */

        /* Vector types - expose as structured arrays */
        case C_LIGHTUSD_VALUE_INT2: return "ii";
        case C_LIGHTUSD_VALUE_INT3: return "iii";
        case C_LIGHTUSD_VALUE_INT4: return "iiii";
        case C_LIGHTUSD_VALUE_FLOAT2: return "ff";
        case C_LIGHTUSD_VALUE_FLOAT3: return "fff";
        case C_LIGHTUSD_VALUE_FLOAT4: return "ffff";
        case C_LIGHTUSD_VALUE_DOUBLE2: return "dd";
        case C_LIGHTUSD_VALUE_DOUBLE3: return "ddd";
        case C_LIGHTUSD_VALUE_DOUBLE4: return "dddd";

        default: return "B";  /* Raw bytes as fallback */
    }
}

/* Buffer protocol implementation */
static int
LightUSDValueArray_getbuffer(LightUSDValueArrayObject *self, Py_buffer *view, int flags)
{
    if (view == NULL) {
        PyErr_SetString(PyExc_ValueError, "NULL view in getbuffer");
        return -1;
    }

    if ((flags & PyBUF_WRITABLE) && self->readonly) {
        PyErr_SetString(PyExc_BufferError, "Array is readonly");
        return -1;
    }

    const char *format = get_format_string(self->value_type);

    /* Fill in the buffer info */
    view->obj = (PyObject *)self;
    view->buf = self->data;
    view->len = self->length * self->itemsize;
    view->readonly = self->readonly;
    view->itemsize = self->itemsize;
    view->format = (flags & PyBUF_FORMAT) ? (char *)format : NULL;
    view->ndim = 1;
    view->shape = (flags & PyBUF_ND) ? &self->length : NULL;
    view->strides = (flags & PyBUF_STRIDES) ? &self->itemsize : NULL;
    view->suboffsets = NULL;
    view->internal = NULL;

    Py_INCREF(self);
    return 0;
}

static void
LightUSDValueArray_releasebuffer(LightUSDValueArrayObject *self, Py_buffer *view)
{
    /* Nothing to do - data is managed by owner object */
}

static PyBufferProcs LightUSDValueArray_as_buffer = {
    .bf_getbuffer = (getbufferproc)LightUSDValueArray_getbuffer,
    .bf_releasebuffer = (releasebufferproc)LightUSDValueArray_releasebuffer,
};

static PyObject *
LightUSDValueArray_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    LightUSDValueArrayObject *self;
    self = (LightUSDValueArrayObject *)type->tp_alloc(type, 0);
    if (self != NULL) {
        self->data = NULL;
        self->length = 0;
        self->itemsize = 0;
        self->readonly = 1;
        self->format = NULL;
        self->value_type = C_LIGHTUSD_VALUE_UNKNOWN;
        self->owner = NULL;
    }
    return (PyObject *)self;
}

static PyObject *
LightUSDValueArray_repr(LightUSDValueArrayObject *self)
{
    return PyUnicode_FromFormat("<ValueArray type=%s length=%zd itemsize=%zd>",
                                c_lightusd_value_type_name(self->value_type),
                                self->length,
                                self->itemsize);
}

static Py_ssize_t
LightUSDValueArray_length(LightUSDValueArrayObject *self)
{
    return self->length;
}

static PySequenceMethods LightUSDValueArray_as_sequence = {
    .sq_length = (lenfunc)LightUSDValueArray_length,
};

static PyTypeObject LightUSDValueArrayType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "lightusd_abi3.ValueArray",
    .tp_doc = "LightUSD value array with buffer protocol support",
    .tp_basicsize = sizeof(LightUSDValueArrayObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = LightUSDValueArray_new,
    .tp_dealloc = (destructor)LightUSDValueArray_dealloc,
    .tp_repr = (reprfunc)LightUSDValueArray_repr,
    .tp_as_buffer = &LightUSDValueArray_as_buffer,
    .tp_as_sequence = &LightUSDValueArray_as_sequence,
};

/* ============================================================================
 * Value Object
 * ============================================================================ */

typedef struct {
    PyObject_HEAD
    CLightUSDValue *value;  /* Managed by RAII on C++ side */
} LightUSDValueObject;

static void
LightUSDValue_dealloc(LightUSDValueObject *self)
{
    if (self->value) {
        c_lightusd_value_free(self->value);
        self->value = NULL;
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
LightUSDValue_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    LightUSDValueObject *self;
    self = (LightUSDValueObject *)type->tp_alloc(type, 0);
    if (self != NULL) {
        self->value = c_lightusd_value_new_null();
        if (self->value == NULL) {
            Py_DECREF(self);
            PyErr_SetString(PyExc_MemoryError, "Failed to create value");
            return NULL;
        }
    }
    return (PyObject *)self;
}

static PyObject *
LightUSDValue_get_type(LightUSDValueObject *self, void *closure)
{
    CLightUSDValueType vtype = c_lightusd_value_type(self->value);
    const char *type_name = c_lightusd_value_type_name(vtype);
    return PyUnicode_FromString(type_name);
}

static PyObject *
LightUSDValue_to_string(LightUSDValueObject *self, PyObject *Py_UNUSED(ignored))
{
    c_lightusd_string_t *str = c_lightusd_string_new_empty();
    if (!str) {
        return PyErr_NoMemory();
    }

    if (!c_lightusd_value_to_string(self->value, str)) {
        c_lightusd_string_free(str);
        PyErr_SetString(PyExc_RuntimeError, "Failed to convert value to string");
        return NULL;
    }

    const char *cstr = c_lightusd_string_str(str);
    PyObject *result = PyUnicode_FromString(cstr);
    c_lightusd_string_free(str);
    return result;
}

static PyObject *
LightUSDValue_as_int(LightUSDValueObject *self, PyObject *Py_UNUSED(ignored))
{
    int val;
    if (!c_lightusd_value_as_int(self->value, &val)) {
        PyErr_SetString(PyExc_TypeError, "Value is not an integer");
        return NULL;
    }
    return PyLong_FromLong(val);
}

static PyObject *
LightUSDValue_as_float(LightUSDValueObject *self, PyObject *Py_UNUSED(ignored))
{
    float val;
    if (!c_lightusd_value_as_float(self->value, &val)) {
        PyErr_SetString(PyExc_TypeError, "Value is not a float");
        return NULL;
    }
    return PyFloat_FromDouble((double)val);
}

static PyObject *
LightUSDValue_from_int(PyTypeObject *type, PyObject *args)
{
    int val;
    if (!PyArg_ParseTuple(args, "i", &val)) {
        return NULL;
    }

    LightUSDValueObject *self = (LightUSDValueObject *)type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }

    self->value = c_lightusd_value_new_int(val);
    if (self->value == NULL) {
        Py_DECREF(self);
        return PyErr_NoMemory();
    }

    return (PyObject *)self;
}

static PyObject *
LightUSDValue_from_float(PyTypeObject *type, PyObject *args)
{
    float val;
    if (!PyArg_ParseTuple(args, "f", &val)) {
        return NULL;
    }

    LightUSDValueObject *self = (LightUSDValueObject *)type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }

    self->value = c_lightusd_value_new_float(val);
    if (self->value == NULL) {
        Py_DECREF(self);
        return PyErr_NoMemory();
    }

    return (PyObject *)self;
}

static PyGetSetDef LightUSDValue_getset[] = {
    {"type", (getter)LightUSDValue_get_type, NULL, "Value type", NULL},
    {NULL}
};

static PyMethodDef LightUSDValue_methods[] = {
    {"to_string", (PyCFunction)LightUSDValue_to_string, METH_NOARGS,
     "Convert value to string representation"},
    {"as_int", (PyCFunction)LightUSDValue_as_int, METH_NOARGS,
     "Get value as integer"},
    {"as_float", (PyCFunction)LightUSDValue_as_float, METH_NOARGS,
     "Get value as float"},
    {"from_int", (PyCFunction)LightUSDValue_from_int, METH_VARARGS | METH_CLASS,
     "Create value from integer"},
    {"from_float", (PyCFunction)LightUSDValue_from_float, METH_VARARGS | METH_CLASS,
     "Create value from float"},
    {NULL}
};

static PyTypeObject LightUSDValueType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "lightusd_abi3.Value",
    .tp_doc = "LightUSD value object",
    .tp_basicsize = sizeof(LightUSDValueObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = LightUSDValue_new,
    .tp_dealloc = (destructor)LightUSDValue_dealloc,
    .tp_methods = LightUSDValue_methods,
    .tp_getset = LightUSDValue_getset,
};

/* ============================================================================
 * Prim Object
 * ============================================================================ */

typedef struct {
    PyObject_HEAD
    CLightUSDPrim *prim;  /* Managed by RAII on C++ side */
} LightUSDPrimObject;

static void
LightUSDPrim_dealloc(LightUSDPrimObject *self)
{
    if (self->prim) {
        c_lightusd_prim_free(self->prim);
        self->prim = NULL;
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
LightUSDPrim_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    const char *prim_type = "Xform";
    static char *kwlist[] = {"prim_type", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|s", kwlist, &prim_type)) {
        return NULL;
    }

    LightUSDPrimObject *self = (LightUSDPrimObject *)type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }

    c_lightusd_string_t *err = c_lightusd_string_new_empty();
    self->prim = c_lightusd_prim_new(prim_type, err);

    if (self->prim == NULL) {
        const char *err_str = c_lightusd_string_str(err);
        PyErr_SetString(PyExc_ValueError, err_str);
        c_lightusd_string_free(err);
        Py_DECREF(self);
        return NULL;
    }

    c_lightusd_string_free(err);
    return (PyObject *)self;
}

static PyObject *
LightUSDPrim_get_type(LightUSDPrimObject *self, void *closure)
{
    const char *prim_type = c_lightusd_prim_type(self->prim);
    if (prim_type == NULL) {
        Py_RETURN_NONE;
    }
    return PyUnicode_FromString(prim_type);
}

static PyObject *
LightUSDPrim_get_element_name(LightUSDPrimObject *self, void *closure)
{
    const char *name = c_lightusd_prim_element_name(self->prim);
    if (name == NULL) {
        Py_RETURN_NONE;
    }
    return PyUnicode_FromString(name);
}

static PyGetSetDef LightUSDPrim_getset[] = {
    {"type", (getter)LightUSDPrim_get_type, NULL, "Prim type", NULL},
    {"element_name", (getter)LightUSDPrim_get_element_name, NULL, "Element name", NULL},
    {NULL}
};

static PyObject *
LightUSDPrim_to_string(LightUSDPrimObject *self, PyObject *Py_UNUSED(ignored))
{
    c_lightusd_string_t *str = c_lightusd_string_new_empty();
    if (!str) {
        return PyErr_NoMemory();
    }

    if (!c_lightusd_prim_to_string(self->prim, str)) {
        c_lightusd_string_free(str);
        PyErr_SetString(PyExc_RuntimeError, "Failed to convert prim to string");
        return NULL;
    }

    const char *cstr = c_lightusd_string_str(str);
    PyObject *result = PyUnicode_FromString(cstr);
    c_lightusd_string_free(str);
    return result;
}

static PyMethodDef LightUSDPrim_methods[] = {
    {"to_string", (PyCFunction)LightUSDPrim_to_string, METH_NOARGS,
     "Convert prim to string representation"},
    {NULL}
};

static PyTypeObject LightUSDPrimType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "lightusd_abi3.Prim",
    .tp_doc = "LightUSD Prim object",
    .tp_basicsize = sizeof(LightUSDPrimObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = LightUSDPrim_new,
    .tp_dealloc = (destructor)LightUSDPrim_dealloc,
    .tp_methods = LightUSDPrim_methods,
    .tp_getset = LightUSDPrim_getset,
};

/* ============================================================================
 * Module Functions
 * ============================================================================ */

static PyObject *
lightusd_detect_format(PyObject *self, PyObject *args)
{
    const char *filename;
    if (!PyArg_ParseTuple(args, "s", &filename)) {
        return NULL;
    }

    CLightUSDFormat format = c_lightusd_detect_format(filename);

    const char *format_str;
    switch (format) {
        case C_LIGHTUSD_FORMAT_USDA: format_str = "USDA"; break;
        case C_LIGHTUSD_FORMAT_USDC: format_str = "USDC"; break;
        case C_LIGHTUSD_FORMAT_USDZ: format_str = "USDZ"; break;
        case C_LIGHTUSD_FORMAT_AUTO: format_str = "AUTO"; break;
        default: format_str = "UNKNOWN"; break;
    }

    return PyUnicode_FromString(format_str);
}

static PyMethodDef lightusd_methods[] = {
    {"detect_format", lightusd_detect_format, METH_VARARGS,
     "Detect USD file format from filename"},
    {NULL, NULL, 0, NULL}
};

static PyModuleDef lightusd_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "lightusd_abi3",
    .m_doc = "LightUSD Python bindings using ABI3 (stable API)",
    .m_size = -1,
    .m_methods = lightusd_methods,
};

/* ============================================================================
 * Module Initialization
 * ============================================================================ */

PyMODINIT_FUNC
PyInit_lightusd_abi3(void)
{
    PyObject *m;

    /* Prepare types */
    if (PyType_Ready(&LightUSDStageType) < 0)
        return NULL;
    if (PyType_Ready(&LightUSDPrimType) < 0)
        return NULL;
    if (PyType_Ready(&LightUSDValueType) < 0)
        return NULL;
    if (PyType_Ready(&LightUSDValueArrayType) < 0)
        return NULL;

    /* Create module */
    m = PyModule_Create(&lightusd_module);
    if (m == NULL)
        return NULL;

    /* Add types to module */
    Py_INCREF(&LightUSDStageType);
    if (PyModule_AddObject(m, "Stage", (PyObject *)&LightUSDStageType) < 0) {
        Py_DECREF(&LightUSDStageType);
        Py_DECREF(m);
        return NULL;
    }

    Py_INCREF(&LightUSDPrimType);
    if (PyModule_AddObject(m, "Prim", (PyObject *)&LightUSDPrimType) < 0) {
        Py_DECREF(&LightUSDPrimType);
        Py_DECREF(m);
        return NULL;
    }

    Py_INCREF(&LightUSDValueType);
    if (PyModule_AddObject(m, "Value", (PyObject *)&LightUSDValueType) < 0) {
        Py_DECREF(&LightUSDValueType);
        Py_DECREF(m);
        return NULL;
    }

    Py_INCREF(&LightUSDValueArrayType);
    if (PyModule_AddObject(m, "ValueArray", (PyObject *)&LightUSDValueArrayType) < 0) {
        Py_DECREF(&LightUSDValueArrayType);
        Py_DECREF(m);
        return NULL;
    }

    /* Add version */
    PyModule_AddStringConstant(m, "__version__", "0.1.0");

    return m;
}

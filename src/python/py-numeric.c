/* SPDX-License-Identifier: Apache-2.0 */
#include "py-internal.h"

typedef struct number_source {
  PyObject* sequence;
  Py_buffer buffer;
  int has_buffer, contiguous, big_endian;
  char format;
} number_source;

static int sequence_number(PyObject* value, lu_number* out) {
  if (PyLong_Check(value)) {
    int overflow = 0;
    out->kind = LU_NUMBER_SIGNED;
    out->value.signed_value = PyLong_AsLongLongAndOverflow(value, &overflow);
    if (overflow > 0) {
      out->kind = LU_NUMBER_UNSIGNED;
      out->value.unsigned_value = PyLong_AsUnsignedLongLong(value);
    } else if (overflow < 0) {
      PyErr_SetString(PyExc_OverflowError, "integer is smaller than int64");
    }
  } else {
    out->kind = LU_NUMBER_FLOAT;
    out->value.float_value = PyFloat_AsDouble(value);
  }
  return !PyErr_Occurred();
}

static int number_at(void* context, size_t index, lu_number* out) {
  number_source* source = (number_source*)context;
  if (!source->has_buffer) {
    PyObject* value = PySequence_GetItem(source->sequence, (Py_ssize_t)index);
    int ok;
    if (!value) return 0;
    ok = sequence_number(value, out);
    Py_DECREF(value);
    return ok;
  }
  const Py_buffer* buffer = &source->buffer;
  const unsigned char* data;
  uint64_t bits = 0;
  size_t i, width = (size_t)buffer->itemsize;
  if (source->contiguous) data = (const unsigned char*)buffer->buf + index * width;
  else {
    Py_ssize_t indices[64];
    int dimension;
    for (dimension = buffer->ndim; dimension-- > 0;) {
      indices[dimension] = (Py_ssize_t)(index % (size_t)buffer->shape[dimension]);
      index /= (size_t)buffer->shape[dimension];
    }
    data = (const unsigned char*)PyBuffer_GetPointer(buffer, indices);
  }
  for (i = 0; i < width; ++i) {
    const size_t shift = source->big_endian ? width - 1 - i : i;
    bits |= (uint64_t)data[i] << (shift * 8);
  }
  switch (source->format) {
    case '?': out->kind = LU_NUMBER_UNSIGNED; out->value.unsigned_value = bits != 0; break;
    case 'B': case 'H': case 'I': case 'L': case 'Q':
      out->kind = LU_NUMBER_UNSIGNED; out->value.unsigned_value = bits; break;
    case 'b': case 'h': case 'i': case 'l': case 'q': case 'n':
      if (width < 8 && (bits & (UINT64_C(1) << (width * 8 - 1)))) bits |= UINT64_MAX << (width * 8);
      out->kind = LU_NUMBER_SIGNED;
      memcpy(&out->value.signed_value, &bits, sizeof(bits)); break;
    case 'e': out->kind = LU_NUMBER_FLOAT; out->value.float_value = lu_half_to_float((uint16_t)bits); break;
    case 'f': {
      const uint32_t word = (uint32_t)bits;
      float value;
      memcpy(&value, &word, sizeof(value));
      out->kind = LU_NUMBER_FLOAT; out->value.float_value = value; break;
    }
    case 'd': out->kind = LU_NUMBER_FLOAT; memcpy(&out->value.float_value, &bits, sizeof(bits)); break;
    default: return 0;
  }
  return 1;
}

int lightusd_numbers_from_python(lightusd_state* state, lightusd_type type,
    uint8_t array, size_t count, PyObject* data, lightusd_value** out) {
  number_source source = {0};
  lu_type_descriptor descriptor;
  size_t lanes;
  lightusd_status status;
  if (!lightusd_type_describe(type, array, &descriptor) || !descriptor.storage ||
      !descriptor.components || count > (size_t)PY_SSIZE_T_MAX / descriptor.components) {
    PyErr_SetString(PyExc_ValueError, "invalid numeric shape"); return -1;
  }
  lanes = count * descriptor.components;
  if (PyObject_CheckBuffer(data)) {
    const char* format;
    size_t width;
    const uint16_t endian_probe = 1;
    if (PyObject_GetBuffer(data, &source.buffer, PyBUF_FULL_RO) != 0) return -1;
    source.has_buffer = 1;
    format = source.buffer.format;
    source.big_endian = *(const unsigned char*)&endian_probe == 0;
    if (format && strchr("@=<>!", format[0])) {
      if (*format == '<') source.big_endian = 0;
      else if (*format == '>' || *format == '!') source.big_endian = 1;
      ++format;
    }
    source.format = format ? format[0] : 0;
    width = source.format && strchr("bB?", source.format) ? 1 :
      source.format && strchr("hHe", source.format) ? 2 :
      source.format && strchr("iIf", source.format) ? 4 :
      source.format && strchr("qQd", source.format) ? 8 :
      source.format && strchr("lL", source.format) ? (size_t)source.buffer.itemsize : 0;
    if (!format || !source.format || format[1] || !width || width > 8 ||
        (width != 1 && width != 2 && width != 4 && width != 8) ||
        source.buffer.itemsize != (Py_ssize_t)width || source.buffer.ndim < 0 || source.buffer.ndim > 64 ||
        source.buffer.len < 0 || (size_t)source.buffer.len / width != lanes || (size_t)source.buffer.len % width) {
      PyBuffer_Release(&source.buffer);
      PyErr_SetString(PyExc_TypeError, "unsupported numeric buffer layout"); return -1;
    }
    source.contiguous = PyBuffer_IsContiguous(&source.buffer, 'C');
  } else {
    const Py_ssize_t length = PySequence_Size(data);
    if (length < 0) return -1;
    if ((size_t)length != lanes) { PyErr_SetString(PyExc_ValueError, "numeric component count mismatch"); return -1; }
    source.sequence = data;
  }
  status = lightusd_value_from_numbers(type, array, count, number_at, &source, out);
  if (source.has_buffer) PyBuffer_Release(&source.buffer);
  if (PyErr_Occurred()) return -1;
  if (status != LIGHTUSD_OK) { lightusd_raise(state, status, "numeric value"); return -1; }
  return 0;
}

// SPDX-License-Identifier: Apache-2.0
#include "value.hh"
namespace lightusd { namespace next {
static void AdoptPod(Value& value, const void* data, size_t size) { value.adopt_pod(data, size); }
Value Value::MakePoint3f(float x, float y, float z) {
  Value v;
  v.core_.type = static_cast<uint16_t>(TypeId::Point3f);
  float data[3] = {x, y, z};
  AdoptPod(v, data, sizeof(data));
  return v;
}

Value Value::MakePoint3d(double x, double y, double z) {
  Value v;
  v.core_.type = static_cast<uint16_t>(TypeId::Point3d);
  double data[3] = {x, y, z};
  AdoptPod(v, data, sizeof(data));
  return v;
}

Value Value::MakeVector3f(float x, float y, float z) {
  Value v;
  v.core_.type = static_cast<uint16_t>(TypeId::Vector3f);
  float data[3] = {x, y, z};
  AdoptPod(v, data, sizeof(data));
  return v;
}

Value Value::MakeVector3d(double x, double y, double z) {
  Value v;
  v.core_.type = static_cast<uint16_t>(TypeId::Vector3d);
  double data[3] = {x, y, z};
  AdoptPod(v, data, sizeof(data));
  return v;
}

Value Value::MakeNormal3f(float x, float y, float z) {
  Value v;
  v.core_.type = static_cast<uint16_t>(TypeId::Normal3f);
  float data[3] = {x, y, z};
  AdoptPod(v, data, sizeof(data));
  return v;
}

Value Value::MakeNormal3d(double x, double y, double z) {
  Value v;
  v.core_.type = static_cast<uint16_t>(TypeId::Normal3d);
  double data[3] = {x, y, z};
  AdoptPod(v, data, sizeof(data));
  return v;
}

Value Value::MakeColor3f(float r, float g, float b) {
  Value v;
  v.core_.type = static_cast<uint16_t>(TypeId::Color3f);
  float data[3] = {r, g, b};
  AdoptPod(v, data, sizeof(data));
  return v;
}

Value Value::MakeColor4f(float r, float g, float b, float a) {
  Value v;
  v.core_.type = static_cast<uint16_t>(TypeId::Color4f);
  float data[4] = {r, g, b, a};
  AdoptPod(v, data, sizeof(data));
  return v;
}

Value Value::MakeTexcoord2f(float u, float v_coord) {
  Value val;
  val.core_.type = static_cast<uint16_t>(TypeId::Texcoord2f);
  float data[2] = {u, v_coord};
  AdoptPod(val, data, sizeof(data));
  return val;
}

Value Value::MakeInt2(int32_t x, int32_t y) {
  Value v;
  v.core_.type = static_cast<uint16_t>(TypeId::Int2);
  int32_t data[2] = {x, y};
  AdoptPod(v, data, sizeof(data));
  return v;
}

Value Value::MakeInt3(int32_t x, int32_t y, int32_t z) {
  Value v;
  v.core_.type = static_cast<uint16_t>(TypeId::Int3);
  int32_t data[3] = {x, y, z};
  AdoptPod(v, data, sizeof(data));
  return v;
}

Value Value::MakeInt4(int32_t x, int32_t y, int32_t z, int32_t w) {
  Value v;
  v.core_.type = static_cast<uint16_t>(TypeId::Int4);
  int32_t data[4] = {x, y, z, w};
  AdoptPod(v, data, sizeof(data));
  return v;
}

Value Value::MakeFloat2(float x, float y) {
  Value v;
  v.core_.type = static_cast<uint16_t>(TypeId::Float2);
  float data[2] = {x, y};
  AdoptPod(v, data, sizeof(data));
  return v;
}

Value Value::MakeFloat3(float x, float y, float z) {
  Value v;
  v.core_.type = static_cast<uint16_t>(TypeId::Float3);
  float data[3] = {x, y, z};
  AdoptPod(v, data, sizeof(data));
  return v;
}

Value Value::MakeFloat4(float x, float y, float z, float w) {
  Value v;
  v.core_.type = static_cast<uint16_t>(TypeId::Float4);
  float data[4] = {x, y, z, w};
  AdoptPod(v, data, sizeof(data));
  return v;
}

Value Value::MakeDouble2(double x, double y) {
  Value v;
  v.core_.type = static_cast<uint16_t>(TypeId::Double2);
  double data[2] = {x, y};
  AdoptPod(v, data, sizeof(data));
  return v;
}

Value Value::MakeDouble3(double x, double y, double z) {
  Value v;
  v.core_.type = static_cast<uint16_t>(TypeId::Double3);
  double data[3] = {x, y, z};
  AdoptPod(v, data, sizeof(data));
  return v;
}

Value Value::MakeDouble4(double x, double y, double z, double w) {
  Value v;
  v.core_.type = static_cast<uint16_t>(TypeId::Double4);
  double data[4] = {x, y, z, w};
  AdoptPod(v, data, sizeof(data));
  return v;
}

Value Value::MakeQuatf(float w, float x, float y, float z) {
  Value v;
  v.core_.type = static_cast<uint16_t>(TypeId::Quatf);
  float data[4] = {w, x, y, z};
  AdoptPod(v, data, sizeof(data));
  return v;
}

Value Value::MakeQuatd(double w, double x, double y, double z) {
  Value v;
  v.core_.type = static_cast<uint16_t>(TypeId::Quatd);
  double data[4] = {w, x, y, z};
  AdoptPod(v, data, sizeof(data));
  return v;
}

Value Value::MakeMatrix2f(const float* data) {
  Value v;
  if (!data) return v;
  v.core_.type = static_cast<uint16_t>(TypeId::Matrix2f);
  AdoptPod(v, data, static_cast<size_t>(4) * sizeof(float));
  return v;
}

Value Value::MakeMatrix3f(const float* data) {
  Value v;
  if (!data) return v;
  v.core_.type = static_cast<uint16_t>(TypeId::Matrix3f);
  AdoptPod(v, data, static_cast<size_t>(9) * sizeof(float));
  return v;
}

Value Value::MakeMatrix4f(const float* data) {
  Value v;
  if (!data) return v;
  v.core_.type = static_cast<uint16_t>(TypeId::Matrix4f);
  AdoptPod(v, data, static_cast<size_t>(16) * sizeof(float));
  return v;
}

Value Value::MakeMatrix2d(const double* data) {
  Value v;
  if (!data) return v;
  v.core_.type = static_cast<uint16_t>(TypeId::Matrix2d);
  AdoptPod(v, data, static_cast<size_t>(4) * sizeof(double));
  return v;
}

Value Value::MakeMatrix3d(const double* data) {
  Value v;
  if (!data) return v;
  v.core_.type = static_cast<uint16_t>(TypeId::Matrix3d);
  AdoptPod(v, data, static_cast<size_t>(9) * sizeof(double));
  return v;
}

Value Value::MakeMatrix4d(const double* data) {
  Value v;
  if (!data) return v;
  v.core_.type = static_cast<uint16_t>(TypeId::Matrix4d);
  AdoptPod(v, data, static_cast<size_t>(16) * sizeof(double));
  return v;
}
}}

#include "array-builder.hh"
#include <cstring>
#include <new>
namespace lightusd { namespace next { namespace detail {
ArrayBuilderStorage::ArrayBuilderStorage(size_t width, size_t alignment)
    : width_(width), alignment_(alignment) {}
ArrayBuilderStorage::~ArrayBuilderStorage() { lu_buffer_release(buffer_); }
ArrayBuilderStorage::ArrayBuilderStorage(ArrayBuilderStorage&& b) noexcept
    : buffer_(b.buffer_), data_(b.data_), size_(b.size_), capacity_(b.capacity_),
      failed_(b.failed_), width_(b.width_), alignment_(b.alignment_) {
  b.buffer_ = nullptr; b.data_ = nullptr; b.size_ = b.capacity_ = 0; b.failed_ = false;
}
ArrayBuilderStorage& ArrayBuilderStorage::operator=(ArrayBuilderStorage&& b) noexcept {
  if (this != &b) { this->~ArrayBuilderStorage(); new (this) ArrayBuilderStorage(std::move(b)); }
  return *this;
}
bool ArrayBuilderStorage::reserve(size_t n) {
  if (failed_) return false;
  if (n <= capacity_) return true;
  if (n > size_t(PTRDIFF_MAX) / width_) { failed_ = true; return false; }
  if (!buffer_) buffer_ = lu_buffer_create(n * width_, alignment_, nullptr);
  else if (!lu_buffer_resize(&buffer_, n * width_)) { failed_ = true; return false; }
  if (!buffer_) { failed_ = true; return false; }
  data_ = const_cast<void*>(lu_buffer_data(buffer_)); capacity_ = n;
  return true;
}
bool ArrayBuilderStorage::resize(size_t n) {
  if (!reserve(n)) return false;
  if (n > size_) std::memset(static_cast<unsigned char*>(data_) + size_ * width_, 0, (n - size_) * width_);
  size_ = n; return true;
}
void ArrayBuilderStorage::append(const void* data, size_t n) {
  if (n > SIZE_MAX - size_ || !reserve(size_ + n)) { failed_ = true; return; }
  if (n) std::memcpy(static_cast<unsigned char*>(data_) + size_ * width_, data, n * width_);
  size_ += n;
}
Value ArrayBuilderStorage::take(TypeId type) {
  Value v;
  const size_t components = GetComponentCount(type);
  if (failed_ || !components || size_ % components) return v;
  if (!buffer_) buffer_ = lu_buffer_create(0, alignment_, nullptr);
  if (!buffer_ || !lu_buffer_resize(&buffer_, size_ * width_)) return v;
  if (lu_value_set_array_buffer(v.mutable_core(), static_cast<uint16_t>(type), buffer_, size_ / components)) {
    lu_buffer_release(buffer_); buffer_ = nullptr; data_ = nullptr; size_ = capacity_ = 0;
  }
  return v;
}
}}} // namespace lightusd::next::detail

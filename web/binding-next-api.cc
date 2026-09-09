// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-api.h"
#include <cstdlib>
#include <emscripten/emscripten.h>
#include <emscripten/val.h>

namespace {
struct Slot {
  void* object;
  uint32_t generation;
  uint32_t kind;
};
Slot* slots = nullptr;
uint32_t slot_count = 0;
constexpr uint32_t index_bits = 20;
constexpr uint32_t index_mask = (1u << index_bits) - 1u;
constexpr uint32_t max_generation = (1u << (32u - index_bits)) - 1u;

Slot* Lookup(uint32_t handle) {
  const uint32_t index = handle & index_mask;
  if (!index || index > slot_count) return nullptr;
  Slot* slot = &slots[index - 1];
  return slot->object && slot->generation == (handle >> index_bits) ? slot : nullptr;
}
}  // namespace

extern "C" {
EMSCRIPTEN_KEEPALIVE uint32_t lightusd_next_create(uint32_t kind) {
  using namespace lightusd::web_next;
  if (kind < 1 || kind > 4) return 0;
  uint32_t index = 0;
  while (index < slot_count && (slots[index].object || !slots[index].generation)) ++index;
  if (index == slot_count) {
    if (slot_count == index_mask) return 0;
    void* allocation = std::realloc(slots, (size_t(slot_count) + 1) * sizeof(Slot));
    if (!allocation) return 0;
    slots = static_cast<Slot*>(allocation);
    slots[slot_count++] = Slot{nullptr, 1, 0};
  }
  void* object = NextCreateObject(kind);
  if (!object) return 0;
  slots[index].object = object;
  slots[index].kind = kind;
  return (slots[index].generation << index_bits) | (index + 1);
}

EMSCRIPTEN_KEEPALIVE void lightusd_next_destroy(uint32_t handle) {
  Slot* slot = Lookup(handle);
  if (!slot) return;
  void* object = slot->object;
  const uint32_t kind = slot->kind;
  slot->object = nullptr;
  // Never wrap: a retired slot cannot make an old handle valid again.
  slot->generation = slot->generation == max_generation ? 0 : slot->generation + 1;
  lightusd::web_next::NextDestroyObject(kind, object);
}

EMSCRIPTEN_KEEPALIVE uint32_t lightusd_next_call(
    uint32_t handle, uint32_t method, uint32_t args) {
  if (!args) return 0;
  if (!handle) return lightusd::web_next::NextInvokeObject(0, nullptr, method, args);
  const Slot* slot = Lookup(handle);
  if (!slot) return 0;
  return lightusd::web_next::NextInvokeObject(slot->kind, slot->object, method, args);
}

EMSCRIPTEN_KEEPALIVE void lightusd_next_val_release(uint32_t value) {
  emscripten::internal::_emval_decref(
      reinterpret_cast<emscripten::EM_VAL>(static_cast<uintptr_t>(value)));
}
}

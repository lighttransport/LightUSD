// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment, Inc.
//
// Shared template bodies for ListPrims/ListShaders and their iterative
// traversal helpers. Included (inside namespace lightusd::tydra) by
// scene-access.cc and by the per-group instantiation TUs
// scene-access-listprims-inst.cc / scene-access-listshaders-inst.cc.
//
// extern template declarations in scene-access.hh suppress implicit
// instantiation in including TUs; the inst TUs emit the symbols explicitly.

template <typename T>
bool TraverseIterative(const lightusd::Prim &root_prim, PathPrimMap<T> &itemmap,
                       size_t max_iter = kMaxDefaultTraversalLimit) {
  // Stack stores: (prim pointer, child index, path length before this prim)
  StackVector<std::tuple<const lightusd::Prim *, size_t, size_t>, 4> stack;
  stack.reserve(64);

  // Shared path buffer - reuse to avoid allocations
  std::string path_buffer;
  path_buffer.reserve(256);

  // Process root prim
  path_buffer = "/" + root_prim.local_path().full_path_name();

  if (root_prim.is<T>()) {
    if (const T *pv = root_prim.as<T>()) {
      DCOUT("Path : <" << path_buffer << "> is " << lightusd::value::TypeTraits<T>::type_name());
      itemmap[path_buffer] = pv;
    }
  }

  if (!root_prim.children().empty()) {
    stack.emplace_back(&root_prim, 0, 0);  // path_len=0 since "/" is implicit
  }

  size_t iter = 0;
  while (!stack.empty()) {
    if (iter++ >= max_iter) break;
    auto &top = stack.back();
    const lightusd::Prim *parent = std::get<0>(top);
    size_t &child_idx = std::get<1>(top);
    const size_t parent_path_len = std::get<2>(top);

    if (child_idx >= parent->children().size()) {
      // All children processed, backtrack
      // Restore path to parent's length
      path_buffer.resize(parent_path_len);
      stack.pop_back();
      continue;
    }

    const lightusd::Prim &child = parent->children()[child_idx];
    ++child_idx;

    // Build path for this child
    size_t current_path_len = path_buffer.size();
    path_buffer += "/";
    path_buffer += child.local_path().full_path_name();

    // Check and add to map if type matches
    if (child.is<T>()) {
      if (const T *pv = child.as<T>()) {
        DCOUT("Path : <" << path_buffer << "> is " << lightusd::value::TypeTraits<T>::type_name());
        itemmap[path_buffer] = pv;
      }
    }

    // Push child to stack if it has children
    if (!child.children().empty()) {
      stack.emplace_back(&child, 0, current_path_len);
    } else {
      // No children, restore path immediately
      path_buffer.resize(current_path_len);
    }
  }

  return true;
}

template <typename ShaderTy>
bool TraverseShaderIterative(const lightusd::Prim &root_prim,
                             PathShaderMap<ShaderTy> &itemmap,
                             size_t max_iter = kMaxDefaultTraversalLimit) {
  // Stack stores: (prim pointer, child index, path length before this prim)
  StackVector<std::tuple<const lightusd::Prim *, size_t, size_t>, 4> stack;
  stack.reserve(64);

  // Shared path buffer - reuse to avoid allocations
  std::string path_buffer;
  path_buffer.reserve(256);

  // Process root prim
  path_buffer = "/" + root_prim.local_path().full_path_name();

  // Check if root is a Shader of the wanted type
  if (const Shader *ps = root_prim.as<Shader>()) {
    if (const ShaderTy *s = ps->value.as<ShaderTy>()) {
      itemmap[path_buffer] = std::make_pair(ps, s);
    }
  }

  if (!root_prim.children().empty()) {
    stack.emplace_back(&root_prim, 0, 0);
  }

  size_t iter = 0;
  while (!stack.empty()) {
    if (iter++ >= max_iter) break;
    auto &top = stack.back();
    const lightusd::Prim *parent = std::get<0>(top);
    size_t &child_idx = std::get<1>(top);
    const size_t parent_path_len = std::get<2>(top);

    if (child_idx >= parent->children().size()) {
      // All children processed, backtrack
      path_buffer.resize(parent_path_len);
      stack.pop_back();
      continue;
    }

    const lightusd::Prim &child = parent->children()[child_idx];
    ++child_idx;

    // Build path for this child
    size_t current_path_len = path_buffer.size();
    path_buffer += "/";
    path_buffer += child.local_path().full_path_name();

    // Check if this is a Shader of the wanted type
    if (const Shader *ps = child.as<Shader>()) {
      if (const ShaderTy *s = ps->value.as<ShaderTy>()) {
        itemmap[path_buffer] = std::make_pair(ps, s);
      }
    }

    // Push child to stack if it has children
    if (!child.children().empty()) {
      stack.emplace_back(&child, 0, current_path_len);
    } else {
      // No children, restore path immediately
      path_buffer.resize(current_path_len);
    }
  }

  return true;
}

template <typename T>
bool ListPrims(const lightusd::Stage &stage, PathPrimMap<T> &m /* output */) {
  // Should report error at compilation stege.
  static_assert(
      (value::TypeId::TYPE_ID_MODEL_BEGIN <= value::TypeTraits<T>::type_id()) &&
          (value::TypeId::TYPE_ID_MODEL_END > value::TypeTraits<T>::type_id()),
      "Not a Prim type.");

  // Check at runtime. Just in case...
  if ((value::TypeId::TYPE_ID_MODEL_BEGIN <= value::TypeTraits<T>::type_id()) &&
      (value::TypeId::TYPE_ID_MODEL_END > value::TypeTraits<T>::type_id())) {
    // Ok
  } else {
    return false;
  }

  for (const auto &root_prim : stage.root_prims()) {
    TraverseIterative(root_prim, m);
  }

  return true;
}

template <typename T>
bool ListShaders(const lightusd::Stage &stage,
                 PathShaderMap<T> &m /* output */) {
  // Concrete Shader type(e.g. UsdPreviewSurface) is classified as Imaging
  // Should report error at compilation stege.
  static_assert((value::TypeId::TYPE_ID_IMAGING_BEGIN <=
                 value::TypeTraits<T>::type_id()) &&
                    (value::TypeId::TYPE_ID_IMAGING_END >
                     value::TypeTraits<T>::type_id()),
                "Not a Shader type.");

  // Check at runtime. Just in case...
  if ((value::TypeId::TYPE_ID_IMAGING_BEGIN <=
       value::TypeTraits<T>::type_id()) &&
      (value::TypeId::TYPE_ID_IMAGING_END > value::TypeTraits<T>::type_id())) {
    // Ok
  } else {
    return false;
  }

  for (const auto &root_prim : stage.root_prims()) {
    TraverseShaderIterative(root_prim, m);
  }

  return true;
}

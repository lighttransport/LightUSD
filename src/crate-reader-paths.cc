// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-2022 Syoyo Fujita.
// Copyright 2023-Present Light Transport Entertainment Inc.
//
// Path decompression and scene graph hierarchy building for CrateReader
//

#ifdef _MSC_VER
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "crate-reader.hh"

#ifdef __wasi__
#else
#include <thread>
#endif

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <stack>

#include "crate-format.hh"
#include "parser-timing.hh"
#include "crate-pprint.hh"
#include "integerCoding.h"
#include "lz4-compression.hh"
#include "memory-budget.hh"
#include "path-util.hh"
#include "pprint-meta.hh"
#include "core/prim-spec.hh"
#include "stream-reader.hh"
#include "lightusd.hh"
#include "value-pprint.hh"
#include "value-types.hh"
#include "tiny-format.hh"
#include "str-util.hh"
#include "safe-arithmetic.hh"

//
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#include "nonstd/expected.hpp"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

//

#include "common-macros.inc"

namespace lightusd {
namespace crate {

#define kTag "[Crate]"

#define CHECK_MEMORY_USAGE(__nbytes) \
  MEMORY_BUDGET_CHECK((*memory_manager_), (__nbytes), kTag)

#if defined(LIGHTUSD_CRATE_USE_FOR_BASED_PATH_INDEX_DECODER)
bool CrateReader::BuildDecompressedPathsImpl(
    BuildDecompressedPathsArg *arg) {

  if (!arg) {
    return false;
  }

  Path parentPath = arg->parentPath;
  if (!arg->pathIndexes) {
    return false;
  }
  if (!arg->elementTokenIndexes) {
    return false;
  }
  if (!arg->jumps) {
    return false;
  }
  if (!arg->visit_table) {
    return false;
  }
  auto &pathIndexes = *arg->pathIndexes;
  auto &elementTokenIndexes = *arg->elementTokenIndexes;
  auto &jumps = *arg->jumps;
  auto &visit_table = *arg->visit_table;

  auto rootPath = Path::make_root_path();

  const size_t maxIter = _config.maxPathIndicesDecodeIteration;

  std::stack<size_t> startIndexStack;
  std::stack<size_t> endIndexStack;
  std::stack<Path> parentPathStack;

  size_t nIter = 0;

  size_t startIndex = arg->startIndex;
  size_t endIndex = arg->endIndex;

  while (nIter < maxIter) {

    DCOUT("startIndex = " << startIndex << ", endIdx = " << endIndex);

    for (size_t thisIndex = startIndex; thisIndex < (endIndex + 1); thisIndex++) {
      //auto thisIndex = curIndex++;
      DCOUT("thisIndex = " << thisIndex << ", pathIndexes.size = " << pathIndexes.size());
      if (parentPath.is_empty()) {
        // root node.
        // Assume single root node in the scene.
        parentPath = rootPath;

        if (thisIndex >= pathIndexes.size()) {
          PUSH_ERROR("Index exceeds pathIndexes.size()");
          return false;
        }

        size_t idx = pathIndexes[thisIndex];
        DCOUT("paths[" << idx << "] is parent. name = " << parentPath.full_path_name());
        if (idx >= _paths.size()) {
          PUSH_ERROR("Index is out-of-range");
          return false;
        }

        if (idx < visit_table.size()) {
          if (visit_table[idx]) {
            PUSH_ERROR_AND_RETURN_TAG(kTag, fmt::format("Circular referencing of Path index {}(thisIndex {}) detected. Invalid Paths data.", idx, thisIndex));
          }
        }

        _paths[idx] = parentPath;
        visit_table[idx] = true;
      } else {
        if (thisIndex >= elementTokenIndexes.size()) {
          PUSH_ERROR("Index exceeds elementTokenIndexes.size()");
          return false;
        }
        int32_t _tokenIndex = elementTokenIndexes[thisIndex];
        DCOUT("elementTokenIndex = " << _tokenIndex);
        bool isPrimPropertyPath = _tokenIndex < 0;
        // Guard against INT32_MIN: -INT32_MIN is UB (signed overflow).
        if (isPrimPropertyPath && _tokenIndex == (std::numeric_limits<int32_t>::min)()) {
          PUSH_ERROR("Invalid tokenIndex (INT32_MIN) in BuildDecompressedPathsImpl.");
          return false;
        }
        // ~0 returns -2147483648, so cast to uint32
        uint32_t tokenIndex = uint32_t(isPrimPropertyPath ? -_tokenIndex : _tokenIndex);

        DCOUT("tokenIndex = " << tokenIndex << ", _tokens.size = " << _tokens.size());
        if (tokenIndex >= _tokens.size()) {
          PUSH_ERROR("Invalid tokenIndex in BuildDecompressedPathsImpl.");
          return false;
        }
        auto const &elemToken = _tokens[size_t(tokenIndex)];
        DCOUT("elemToken = " << elemToken);

        if (thisIndex >= pathIndexes.size()) {
          PUSH_ERROR("thisIndex exceeds pathIndexes.size()");
          return false;
        }

        size_t idx = pathIndexes[thisIndex];
        DCOUT("[" << idx << "].append = " << elemToken);

        if (idx >= _paths.size()) {
          PUSH_ERROR("Index is out-of-range");
          return false;
        }

        if (idx >= _elemPaths.size()) {
          PUSH_ERROR("Index is out-of-range");
          return false;
        }

        if (idx < visit_table.size()) {
          if (visit_table[idx]) {
            PUSH_ERROR_AND_RETURN_TAG(kTag, fmt::format("Circular referencing of Path index {}(thisIndex {}) detected. Invalid Paths data.", idx, thisIndex));
          }
        }

        // Reconstruct full path
        _paths[idx] =
            isPrimPropertyPath ? parentPath.AppendProperty(elemToken.str())
                               : parentPath.AppendElement(elemToken.str()); // prim, variantSelection, etc.

        // also set leaf path for 'primChildren' check
        _elemPaths[idx] = Path(elemToken.str(), "");
        //_paths[pathIndexes[thisIndex]].SetLocalPart(elemToken.str());

        visit_table[idx] = true;
      }

      // If we have either a child or a sibling but not both, then just
      // continue to the neighbor.  If we have both then spawn a task for the
      // sibling and do the child ourself.  We think that our path trees tend
      // to be broader more often than deep.

      if (thisIndex >= jumps.size()) {
        PUSH_ERROR("Index is out-of-range");
        return false;
      }

      bool hasChild = (jumps[thisIndex] > 0) || (jumps[thisIndex] == -1);
      bool hasSibling = (jumps[thisIndex] >= 0);
      DCOUT("hasChild = " << hasChild << ", hasSibling = " << hasSibling);

      if (hasChild) {
        if (hasSibling) {
          auto siblingIndex = thisIndex + size_t(jumps[thisIndex]);

          if (siblingIndex >= jumps.size()) {
            PUSH_ERROR_AND_RETURN("jump index corrupted.");
          }

          // Find subtree end.
          size_t subtreeStartIdx = siblingIndex;
          size_t subtreeIdx = subtreeStartIdx;

          for (; subtreeIdx < jumps.size(); subtreeIdx++) {

            bool has_child = (jumps[subtreeIdx] > 0) || (jumps[subtreeIdx] == -1);
            bool has_sibling = (jumps[subtreeIdx] >= 0);

            if (has_child || has_sibling) {
              continue;
            }
            break;
          }

          size_t subtreeEndIdx = subtreeIdx;
          if (subtreeEndIdx >= jumps.size()) {
            // Guess corrupted.
            PUSH_ERROR_AND_RETURN("jump indices seems corrupted.");
          }

          DCOUT("subtree startIdx " << subtreeStartIdx << ", subtree endIndex " << subtreeEndIdx);

          if (subtreeEndIdx >= subtreeStartIdx) {

            // index range after traversing subtree
            if (jumps[thisIndex] > 1) {

                // Setup stacks to resume loop from [Cont.]
                startIndexStack.push(thisIndex+1);
                // jumps should be always positive, so no siblingIndex < thisIndex
                endIndexStack.push(siblingIndex-1); // endIndex is inclusive so subtract 1.

                {
                  if (thisIndex >= pathIndexes.size()) {
                    PUSH_ERROR("thisIndex exceeds pathIndexes.size()");
                    return false;
                  }
                  size_t idx = pathIndexes[thisIndex];
                  if (idx >= _paths.size()) {
                    PUSH_ERROR("Index is out-of-range");
                    return false;
                  }

                  parentPathStack.push(_paths[idx]);
                }
            }

            startIndexStack.push(subtreeStartIdx);
            endIndexStack.push(subtreeEndIdx);

            parentPathStack.push(parentPath);
            DCOUT("stack size: " << startIndexStack.size());

            nIter++;

            break; // goto `(A)`
          }

        }

        // [Cont.]
        if (thisIndex >= pathIndexes.size()) {
          PUSH_ERROR("thisIndex exceeds pathIndexes.size()");
          return false;
        }
        size_t idx = pathIndexes[thisIndex];
        if (idx >= _paths.size()) {
          PUSH_ERROR("Index is out-of-range");
          return false;
        }

        parentPath = _paths[idx];

      }
    }

    // (A)

    if (startIndexStack.empty()) {
      break; // end traversal
    }

    startIndex = startIndexStack.top();
    startIndexStack.pop();

    endIndex = endIndexStack.top();
    endIndexStack.pop();

    parentPath = parentPathStack.top();
    parentPathStack.pop();

    nIter++;
  }

  if (nIter >= maxIter) {
    PUSH_ERROR_AND_RETURN("PathIndex tree Too deep.");
  }

  return true;
}
#else
bool CrateReader::BuildDecompressedPathsImpl(
    std::vector<uint32_t> const &pathIndexes,
    std::vector<int32_t> const &elementTokenIndexes,
    std::vector<int32_t> const &jumps,
    std::vector<bool> &visit_table,
    size_t curIndex, const Path &_parentPath) {

  Path parentPath = _parentPath;

  bool hasChild = false, hasSibling = false;
  do {
    auto thisIndex = curIndex++;
    DCOUT("thisIndex = " << thisIndex << ", pathIndexes.size = " << pathIndexes.size());
    if (parentPath.is_empty()) {
      // root node.
      // Assume single root node in the scene.
      parentPath = Path::make_root_path();

      if (thisIndex >= pathIndexes.size()) {
        PUSH_ERROR("Index exceeds pathIndexes.size()");
        return false;
      }

      size_t idx = pathIndexes[thisIndex];
      DCOUT("paths[" << idx << "] is parent. name = " << parentPath.full_path_name());
      if (idx >= _paths.size()) {
        PUSH_ERROR("Index is out-of-range");
        return false;
      }

      if (idx < visit_table.size()) {
        if (visit_table[idx]) {
          PUSH_ERROR_AND_RETURN_TAG(kTag, "Circular referencing of Path index tree detected. Invalid Paths data.");
        }
      }

      _paths[idx] = parentPath;
      visit_table[idx] = true;
    } else {
      if (thisIndex >= elementTokenIndexes.size()) {
        PUSH_ERROR("Index exceeds elementTokenIndexes.size()");
        return false;
      }
      int32_t _tokenIndex = elementTokenIndexes[thisIndex];
      DCOUT("elementTokenIndex = " << _tokenIndex);
      bool isPrimPropertyPath = _tokenIndex < 0;
      // Guard against INT32_MIN: -INT32_MIN is UB (signed overflow).
      if (isPrimPropertyPath && _tokenIndex == (std::numeric_limits<int32_t>::min)()) {
        PUSH_ERROR("Invalid tokenIndex (INT32_MIN) in BuildDecompressedPathsImpl.");
        return false;
      }
      // ~0 returns -2147483648, so cast to uint32
      uint32_t tokenIndex = uint32_t(isPrimPropertyPath ? -_tokenIndex : _tokenIndex);

      DCOUT("tokenIndex = " << tokenIndex << ", _tokens.size = " << _tokens.size());
      if (tokenIndex >= _tokens.size()) {
        PUSH_ERROR("Invalid tokenIndex in BuildDecompressedPathsImpl.");
        return false;
      }
      auto const &elemToken = _tokens[size_t(tokenIndex)];
      DCOUT("elemToken = " << elemToken);

      if (thisIndex >= pathIndexes.size()) {
        PUSH_ERROR("thisIndex exceeds pathIndexes.size()");
        return false;
      }

      size_t idx = pathIndexes[thisIndex];
      DCOUT("[" << idx << "].append = " << elemToken);

      if (idx >= _paths.size()) {
        PUSH_ERROR("Index is out-of-range");
        return false;
      }

      if (idx >= _elemPaths.size()) {
        PUSH_ERROR("Index is out-of-range");
        return false;
      }

      if (idx < visit_table.size()) {
        if (visit_table[idx]) {
          PUSH_ERROR_AND_RETURN_TAG(kTag, "Circular referencing of Path index tree detected. Invalid Paths data.");
        }
      }

      // Reconstruct full path
      _paths[idx] =
          isPrimPropertyPath ? parentPath.AppendProperty(elemToken.str())
                             : parentPath.AppendElement(elemToken.str()); // prim, variantSelection, etc.

      // also set leaf path for 'primChildren' check
      _elemPaths[idx] = Path(elemToken.str(), "");
      //_paths[pathIndexes[thisIndex]].SetLocalPart(elemToken.str());

      visit_table[idx] = true;
    }

    // If we have either a child or a sibling but not both, then just
    // continue to the neighbor.  If we have both then spawn a task for the
    // sibling and do the child ourself.  We think that our path trees tend
    // to be broader more often than deep.

    if (thisIndex >= jumps.size()) {
      PUSH_ERROR("Index is out-of-range");
      return false;
    }

    hasChild = (jumps[thisIndex] > 0) || (jumps[thisIndex] == -1);
    hasSibling = (jumps[thisIndex] >= 0);
    DCOUT("hasChild = " << hasChild << ", hasSibling = " << hasSibling);

    DCOUT(fmt::format("hasChild {}, hasSibling {}", hasChild, hasSibling));

    if (hasChild) {
      if (hasSibling) {
        auto siblingIndex = thisIndex + size_t(jumps[thisIndex]);
        if (!BuildDecompressedPathsImpl(pathIndexes, elementTokenIndexes, jumps, visit_table,
                                        siblingIndex, parentPath)) {
          return false;
        }
      }

      if (thisIndex >= pathIndexes.size()) {
        PUSH_ERROR("thisIndex exceeds pathIndexes.size()");
        return false;
      }
      size_t idx = pathIndexes[thisIndex];
      if (idx >= _paths.size()) {
        PUSH_ERROR("Index is out-of-range");
        return false;
      }

      // Have a child (may have also had a sibling). Reset parent path.
      parentPath = _paths[idx];
    }
    // If we had only a sibling, we just continue since the parent path is
    // unchanged and the next thing in the reader stream is the sibling's
    // header.
  } while (hasChild || hasSibling);

  return true;
}
#endif

bool CrateReader::ReadCompressedPaths(const uint64_t maxNumPaths) {
  std::vector<uint32_t> pathIndexes;
  std::vector<int32_t> elementTokenIndexes;
  std::vector<int32_t> jumps;

  // Read number of encoded paths.
  uint64_t numEncodedPaths;
  if (!sr()->read8(&numEncodedPaths)) {
    _err += "Failed to read the number of encoded paths.\n";
    return false;
  }

  DCOUT("maxNumPaths : " << maxNumPaths);
  DCOUT("numEncodedPaths : " << numEncodedPaths);

  // Number of compressed paths could be less than maxNumPaths,
  // but should not be greater.
  if (maxNumPaths < numEncodedPaths) {
    _err += "Size mismatch of numEncodedPaths at `PATHS` section.\n";
    return false;
  }


  // Scratch decode arrays are local to this method. Keep their memory budget
  // scoped so malformed streams that return early do not leak the cap counter.
  size_t decode_arrays_bytes{0};
  if (!safe::mul(size_t(numEncodedPaths), sizeof(int32_t) * size_t(3),
                 &decode_arrays_bytes)) {
    PUSH_ERROR_AND_RETURN_TAG(kTag, "Integer overflow in path array size computation.");
  }
  auto decode_arrays_budget = memory_manager_->ReserveScoped(decode_arrays_bytes);
  if (!decode_arrays_budget.IsReserved()) {
    PUSH_ERROR_AND_RETURN_TAG(kTag, "Memory budget exceeded for compressed path arrays.");
  }

  pathIndexes.resize(static_cast<size_t>(numEncodedPaths));
  elementTokenIndexes.resize(static_cast<size_t>(numEncodedPaths));
  jumps.resize(static_cast<size_t>(numEncodedPaths));

  size_t compBufferSize = Usd_IntegerCompression::GetCompressedBufferSize(static_cast<size_t>(numEncodedPaths));
  size_t workspaceBufferSize = Usd_IntegerCompression::GetDecompressionWorkingSpaceSize(static_cast<size_t>(numEncodedPaths));

  if (!ReserveDecompressionBuffers(compBufferSize, workspaceBufferSize)) {
    return false;
  }
  // Create references for compatibility with existing code
  std::vector<char> &compBuffer = decomp_comp_buffer();
  std::vector<char> &workingSpace = decomp_working_buffer();

  // pathIndexes.
  {
    uint64_t compPathIndexesSize;
    if (!sr()->read8(&compPathIndexesSize)) {
      _err += "Failed to read pathIndexesSize.\n";
      return false;
    }

    if (compPathIndexesSize > compBufferSize) {
      PUSH_ERROR_AND_RETURN_TAG(kTag, "Invalid Compressed PathIndexes size.");
    }

    if (!sr()->read_exact(size_t(compPathIndexesSize), size_t(compPathIndexesSize),
                  reinterpret_cast<uint8_t *>(compBuffer.data()))) {
      _err += "Failed to read compressed pathIndexes data.\n";
      return false;
    }

    DCOUT("comBuffer.size = " << compBuffer.size());
    DCOUT("compPathIndexesSize = " << compPathIndexesSize);

    std::string err;
    Usd_IntegerCompression::DecompressFromBuffer(
        compBuffer.data(), size_t(compPathIndexesSize), pathIndexes.data(),
        size_t(numEncodedPaths), &err, workingSpace.data());
    if (!err.empty()) {
      _err += "Failed to decode pathIndexes\n" + err;
      return false;
    }
  }

  // elementTokenIndexes.
  {
    uint64_t compElementTokenIndexesSize;
    if (!sr()->read8(&compElementTokenIndexesSize)) {
      _err += "Failed to read elementTokenIndexesSize.\n";
      return false;
    }

    if (compElementTokenIndexesSize > compBufferSize) {
      PUSH_ERROR_AND_RETURN_TAG(kTag, "Invalid Compressed elementTokenIndexes size.");
    }

    if (!sr()->read_exact(size_t(compElementTokenIndexesSize),
                  size_t(compElementTokenIndexesSize),
                  reinterpret_cast<uint8_t *>(compBuffer.data()))) {
      PUSH_ERROR("Failed to read elementTokenIndexes data.");
      return false;
    }

    std::string err;
    Usd_IntegerCompression::DecompressFromBuffer(
        compBuffer.data(), size_t(compElementTokenIndexesSize),
        elementTokenIndexes.data(), size_t(numEncodedPaths), &err,
        workingSpace.data());

    if (!err.empty()) {
      PUSH_ERROR("Failed to decode elementTokenIndexes.");
      return false;
    }
  }

  // jumps.
  {
    uint64_t compJumpsSize;
    if (!sr()->read8(&compJumpsSize)) {
      PUSH_ERROR("Failed to read compressed jumpsSize.");
      return false;
    }

    if (compJumpsSize > compBufferSize) {
      PUSH_ERROR_AND_RETURN_TAG(kTag, "Invalid Compressed elementTokenIndexes size.");
    }

    if (!sr()->read_exact(size_t(compJumpsSize), size_t(compJumpsSize),
                  reinterpret_cast<uint8_t *>(compBuffer.data()))) {
      PUSH_ERROR("Failed to read compressed jumps data.");
      return false;
    }

    std::string err;
    Usd_IntegerCompression::DecompressFromBuffer(
        compBuffer.data(), size_t(compJumpsSize), jumps.data(), size_t(numEncodedPaths),
        &err, workingSpace.data());

    if (!err.empty()) {
      PUSH_ERROR("Failed to decode jumps.");
      return false;
    }
  }

#ifdef LIGHTUSD_LOCAL_DEBUG_PRINT
  for (size_t i = 0; i < pathIndexes.size(); i++) {
    DCOUT("pathIndexes[" << i << "] = " << pathIndexes[i]);
  }

  for (size_t i = 0; i < elementTokenIndexes.size(); i++) {
    std::stringstream ss;
    ss << "elementTokenIndexes[" << i << "] = " << elementTokenIndexes[i];
    int32_t tokIdx = elementTokenIndexes[i];
    if (tokIdx < 0) {
      // Property Path. Need to negate it.
      tokIdx = -tokIdx;
    }
    if (auto tokv = GetToken(crate::Index(uint32_t(tokIdx)))) {
      ss << "(" << tokv.value() << ")";
    }
    ss << "\n";
    DCOUT(ss.str());
  }

  for (size_t i = 0; i < jumps.size(); i++) {
    DCOUT(fmt::format("jumps[{}] = {}", i, jumps[i]));
  }
#endif

  // For circular tree check
  std::vector<bool> visit_table;
  auto visit_table_budget = memory_manager_->ReserveScoped(_paths.size());
  if (!visit_table_budget.IsReserved()) {
    PUSH_ERROR_AND_RETURN_TAG(kTag, "Memory budget exceeded for path visit table.");
  }

  // `_paths` is already initialized just before calling this ReadCompressedPaths
  visit_table.resize(_paths.size());
  for (size_t i = 0; i < visit_table.size(); i++) {
    visit_table[i] = false;
  }

  // Now build the paths.
#if defined(LIGHTUSD_CRATE_USE_FOR_BASED_PATH_INDEX_DECODER)
  BuildDecompressedPathsArg arg;
  arg.pathIndexes = &pathIndexes;
  arg.elementTokenIndexes = &elementTokenIndexes;
  arg.jumps = &jumps;
  arg.visit_table = &visit_table;
  if (pathIndexes.empty()) {
    PUSH_ERROR("pathIndexes is empty.");
    return false;
  }
  arg.startIndex = 0;
  arg.endIndex = pathIndexes.size() - 1; // or numEncodedPaths - 1
  arg.parentPath = Path();
  if (!BuildDecompressedPathsImpl(&arg)) {
    return false;
  }

#else
  if (!BuildDecompressedPathsImpl(pathIndexes, elementTokenIndexes, jumps, visit_table,
                                  /* curIndex */ 0, Path())) {
    return false;
  }
#endif

  //
  // Ensure decoded numEncodedPaths.
  //
  size_t sumDecodedPaths = 0;
  for (size_t i = 0; i < visit_table.size(); i++) {
    if (visit_table[i]) {
      sumDecodedPaths++;
    }
  }
  if (sumDecodedPaths != numEncodedPaths) {
    PUSH_ERROR_AND_RETURN(fmt::format("Decoded {} paths but numEncodedPaths in Crate is {}. Possible corruption of Crate data.",
      sumDecodedPaths, numEncodedPaths));
  }

  // Now build node hierarchy.

  // Circular referencing check should be done in BuildDecompressedPathsImpl,
  // but do check it again just in case.
  for (size_t i = 0; i < visit_table.size(); i++) {
    visit_table[i] = false;
  }

  auto build_node_hierarchy_from_decoded_paths = [&]() -> bool {
    std::unordered_map<std::string, size_t> path_to_index;
    {
      size_t path_to_index_bytes;
      if (!safe::mul(_paths.size(), sizeof(std::string) + 64, &path_to_index_bytes)) {
        PUSH_ERROR_AND_RETURN_TAG(kTag, "Integer overflow: _paths.size() * (sizeof(std::string) + 64)");
      }
      CHECK_MEMORY_USAGE(path_to_index_bytes);
    }
    path_to_index.reserve(_paths.size());

    for (size_t path_idx = 0; path_idx < _paths.size(); ++path_idx) {
      if (!_paths[path_idx].is_valid()) {
        continue;
      }
      path_to_index.emplace(_paths[path_idx].full_path_name(), path_idx);
    }

    std::function<bool(size_t)> build_one = [&](const size_t path_idx) -> bool {
      if (path_idx >= _paths.size() || path_idx >= _nodes.size() ||
          path_idx >= visit_table.size()) {
        PUSH_ERROR_AND_RETURN_TAG(kTag, "PathIndex out-of-range.");
      }
      if (!_paths[path_idx].is_valid()) {
        PUSH_ERROR_AND_RETURN_TAG(kTag, "Invalid decoded path.");
      }
      if (visit_table[path_idx]) {
        return true;
      }
      if (_nodes[path_idx].GetParent() != -2) {
        PUSH_ERROR_AND_RETURN_TAG(kTag,
                                  "Corrupted path hierarchy: duplicate node.");
      }

      int64_t parent_node_index = -1;
      Path parent_path = _paths[path_idx].get_parent_path();
      while (parent_path.is_valid() && !parent_path.full_path_name().empty()) {
        const auto parent_it = path_to_index.find(parent_path.full_path_name());
        if (parent_it != path_to_index.end()) {
          parent_node_index = static_cast<int64_t>(parent_it->second);
          if (!visit_table[parent_it->second] &&
              !build_one(parent_it->second)) {
            return false;
          }
          break;
        }

        const Path next_parent = parent_path.get_parent_path();
        if (!next_parent.is_valid() ||
            next_parent.full_path_name() == parent_path.full_path_name()) {
          break;
        }
        parent_path = next_parent;
      }

      Node node(parent_node_index, _paths[path_idx]);
      if (path_idx < _elemPaths.size()) {
        node.SetElementPath(_elemPaths[path_idx]);
      }
      _nodes[path_idx] = node;
      visit_table[path_idx] = true;

      if (parent_node_index >= 0) {
        const size_t parent_idx = static_cast<size_t>(parent_node_index);
        if (parent_idx >= _nodes.size()) {
          PUSH_ERROR_AND_RETURN_TAG(kTag, "Parent node index out-of-range.");
        }
        std::string child_name;
        if (path_idx < _elemPaths.size() && _elemPaths[path_idx].is_valid() &&
            !_elemPaths[path_idx].is_empty()) {
          child_name = _elemPaths[path_idx].full_path_name();
        } else {
          child_name = _paths[path_idx].full_path_name();
        }
        if (!_nodes[parent_idx].AddChildren(child_name, path_idx)) {
          PUSH_ERROR_AND_RETURN_TAG(
              kTag,
              fmt::format("Duplicate child `{}` under parent `{}`.",
                          child_name,
                          _paths[parent_idx].full_path_name()));
        }
      }

      return true;
    };

    for (size_t encoded_idx = 0; encoded_idx < pathIndexes.size();
         ++encoded_idx) {
      const size_t path_idx = pathIndexes[encoded_idx];
      if (!build_one(path_idx)) {
        return false;
      }
    }

    return true;
  };

  (void)elementTokenIndexes;
  (void)jumps;
  if (!build_node_hierarchy_from_decoded_paths()) {
    return false;
  }

  sumDecodedPaths = 0;
  for (size_t i = 0; i < visit_table.size(); i++) {
    if (visit_table[i]) {
      sumDecodedPaths++;
    }
  }
  if (sumDecodedPaths != numEncodedPaths) {
    PUSH_ERROR_AND_RETURN(fmt::format("Decoded {} paths but numEncodedPaths during hierarchy build is {}. Possible corruption of Crate data.",
      sumDecodedPaths, numEncodedPaths));
  }

  return true;
}

} // namespace crate
} // namespace lightusd

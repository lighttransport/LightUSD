// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment, Inc.
//

#include "common-macros.inc"
#include "pprinter.hh"
#include "core/prim.hh"
#include "tiny-container.hh"
#include "scene-access.hh"

#include <sstream>

namespace lightusd {
namespace tydra {

namespace {

// Helper to compute XformNode properties from a Prim
static void ComputeXformNodeProperties(
    const Prim *prim, const Path &parent_abs_path,
    const value::matrix4d &parent_world_mat, const double t,
    const lightusd::value::TimeSampleInterpolationType tinterp,
    XformNode &node) {

  node.element_name = prim->element_name();
  node.absolute_path = parent_abs_path.AppendPrim(prim->element_name());
  node.prim_id = prim->prim_id();
  node.prim = prim;  // Assume Prim's address does not change.

  DCOUT(prim->element_name() << ": IsXformablePrim" << IsXformablePrim(*prim));

  if (IsXformablePrim(*prim)) {
    bool resetXformStack{false};

    value::matrix4d localMat =
        GetLocalTransform(*prim, &resetXformStack, t, tinterp);
    DCOUT("local mat = " << localMat);

    node.has_resetXformStack() = resetXformStack;

    value::matrix4d m;

    if (resetXformStack) {
      // Ignore parent Xform.
      m = localMat;
    } else {
      // matrix is row-major, so local first
      m = localMat * parent_world_mat;
    }

    node.set_parent_world_matrix(parent_world_mat);
    node.set_local_matrix(localMat);
    node.set_world_matrix(m);
    node.has_xform() = true;
  } else {
    DCOUT("Not xformable");
    node.has_xform() = false;
    node.has_resetXformStack() = false;
    node.set_parent_world_matrix(parent_world_mat);
    node.set_world_matrix(parent_world_mat);
    node.set_local_matrix(value::matrix4d::identity());
  }
}

// Iterative version of BuildXformNodeFromStage using explicit stack
bool BuildXformNodeFromStageIterative(
    const lightusd::Stage &stage, const Path &initial_parent_path, const Prim *root_prim,
    XformNode *nodeOut, /* out */
    value::matrix4d rootMat, const double t,
    const lightusd::value::TimeSampleInterpolationType tinterp,
    size_t max_iter = kMaxDefaultTraversalLimit) {

  (void)stage;  // Currently unused

  if (!nodeOut) {
    return false;
  }

  // Stack entry for iterative processing
  struct StackEntry {
    const Prim *prim;
    Path parent_path;
    value::matrix4d parent_world_mat;
    size_t child_idx;
    XformNode node;

    StackEntry(const Prim *p, Path pp, value::matrix4d pwm)
        : prim(p), parent_path(std::move(pp)), parent_world_mat(pwm), child_idx(0) {}
  };

  StackVector<StackEntry, 4> stack;
  stack.reserve(64);

  // Initialize with root prim
  stack.emplace_back(root_prim, initial_parent_path, rootMat);

  // Compute root node properties
  ComputeXformNodeProperties(root_prim, initial_parent_path, rootMat, t, tinterp,
                             stack.back().node);

  size_t iter = 0;
  while (!stack.empty()) {
    if (iter++ >= max_iter) break;
    StackEntry &curr = stack.back();
    const auto &children = curr.prim->children();

    if (curr.child_idx < children.size()) {
      // Push next child
      const Prim &child = children[curr.child_idx];
      curr.child_idx++;

      stack.emplace_back(&child, curr.node.absolute_path, curr.node.get_world_matrix());

      // Compute new child's properties
      StackEntry &new_entry = stack.back();
      ComputeXformNodeProperties(new_entry.prim, new_entry.parent_path,
                                 new_entry.parent_world_mat, t, tinterp,
                                 new_entry.node);
    } else {
      // All children processed
      if (stack.size() > 1) {
        // Move completed node to parent's children
        XformNode completed = std::move(curr.node);
        stack.pop_back();
        // Note: parent pointer will point to stack.back().node, which will be
        // moved later. This preserves the same behavior as the recursive version.
        completed.parent = &stack.back().node;
        stack.back().node.children.emplace_back(std::move(completed));
      } else {
        // Root node - copy to output
        *nodeOut = std::move(curr.node);
        stack.pop_back();
      }
    }
  }

  return true;
}

// Iterative version of DumpXformNode using explicit stack
std::string DumpXformNodeIterative(const XformNode &root,
                                   size_t max_iter = kMaxDefaultTraversalLimit) {
  std::stringstream ss;

  // Stack entry: (node pointer, indent, child index, closing_brace_pending)
  // child_idx == SIZE_MAX means we haven't printed this node yet
  struct StackEntry {
    const XformNode *node;
    uint32_t indent;
    size_t child_idx;
    StackEntry(const XformNode *n, uint32_t i)
        : node(n), indent(i), child_idx(SIZE_MAX) {}
  };

  StackVector<StackEntry, 4> stack;
  stack.reserve(64);
  stack.emplace_back(&root, 0);

  size_t iter = 0;
  while (!stack.empty()) {
    if (iter++ >= max_iter) break;
    StackEntry &entry = stack.back();

    if (entry.child_idx == SIZE_MAX) {
      // First visit: print node info
      ss << pprint::Indent(entry.indent) << "Prim name: " << entry.node->element_name
         << " PrimID: " << entry.node->prim_id << " (Path " << entry.node->absolute_path
         << ") Xformable? " << entry.node->has_xform() << " resetXformStack? "
         << entry.node->has_resetXformStack() << " {\n";
      ss << pprint::Indent(entry.indent + 1)
         << "parent_world: " << entry.node->get_parent_world_matrix() << "\n";
      ss << pprint::Indent(entry.indent + 1) << "world: " << entry.node->get_world_matrix()
         << "\n";
      ss << pprint::Indent(entry.indent + 1) << "local: " << entry.node->get_local_matrix()
         << "\n";
      entry.child_idx = 0;
    }

    // Process children
    const auto &children = entry.node->children;
    if (entry.child_idx < children.size()) {
      size_t idx = entry.child_idx++;
      stack.emplace_back(&children[idx], entry.indent + 1);
    } else {
      // All children processed, print closing brace and pop
      ss << pprint::Indent(entry.indent + 1) << "}\n";
      stack.pop_back();
    }
  }

  return ss.str();
}

}  // namespace local

bool BuildXformNodeFromStage(
    const lightusd::Stage &stage, XformNode *rootNode, /* out */
    const double t,
    const lightusd::value::TimeSampleInterpolationType tinterp) {
  if (!rootNode) {
    return false;
  }

  XformNode stage_root;
  stage_root.element_name = "";  // Stage root element name is empty.
  stage_root.absolute_path = Path("/", "");
  stage_root.has_xform() = false;
  stage_root.parent = nullptr;
  stage_root.prim = nullptr;  // No prim for stage root.
  stage_root.prim_id = -1;
  stage_root.has_resetXformStack() = false;
  stage_root.set_parent_world_matrix(value::matrix4d::identity());
  stage_root.set_world_matrix(value::matrix4d::identity());
  stage_root.set_local_matrix(value::matrix4d::identity());

  for (const auto &root : stage.root_prims()) {
    XformNode node;

    value::matrix4d rootMat{value::matrix4d::identity()};

    if (!BuildXformNodeFromStageIterative(stage, stage_root.absolute_path, &root,
                                          &node, rootMat, t, tinterp)) {
      return false;
    }

    stage_root.children.emplace_back(std::move(node));
  }

  (*rootNode) = stage_root;

  return true;
}

std::string DumpXformNode(const XformNode &node) {
  return DumpXformNodeIterative(node);
}

}  // namespace tydra
}  // namespace lightusd

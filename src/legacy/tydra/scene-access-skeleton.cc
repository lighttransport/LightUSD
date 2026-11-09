// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment, Inc.
//

#include "common-macros.inc"
#include "pprinter.hh"
#include "core/prim.hh"
#include "scene-access.hh"
#include "usdGeom.hh"
#include "usdSkel.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lightusd {
namespace tydra {

#define PushError(msg) \
  if (err) {           \
    *err += msg;       \
  }

namespace detail {

static bool BuildSkelHierarchyImpl(
    /* inout */ SkelNode &rootNode,
    const std::vector<std::vector<size_t>> &childrenMap,
    const std::vector<value::token> &joints,
    const std::vector<value::token> &jointNames,
    const std::vector<value::matrix4d> &bindTransforms,
    const std::vector<value::matrix4d> &restTransforms,
    std::string *err = nullptr) {
  // Iterative traversal using explicit stack to avoid stack overflow on deep hierarchies
  struct StackEntry {
    SkelNode *parent;
    size_t child_list_idx;  // index into childrenMap[parentIdx]
  };

  // Guard: max iterations = total number of joints (each joint is visited exactly once)
  // plus one pop per stack frame. A reasonable upper bound is 2 * joints.size() + 1.
  const size_t kMaxIter = joints.size() * 2 + 1;
  size_t iter = 0;

  std::vector<StackEntry> stack;
  stack.push_back({&rootNode, 0});

  while (!stack.empty()) {
    if (iter++ >= kMaxIter) {
      if (err) {
        (*err) += "BuildSkelHierarchyImpl: exceeded maximum iteration count. "
                  "Possible cycle in skeleton hierarchy.";
      }
      return false;
    }

    auto &top = stack.back();
    size_t parentIdx = size_t(top.parent->joint_id);

    if (parentIdx >= childrenMap.size() ||
        top.child_list_idx >= childrenMap[parentIdx].size()) {
      stack.pop_back();
      continue;
    }

    size_t i = childrenMap[parentIdx][top.child_list_idx];
    top.child_list_idx++;

    DCOUT("add joint " << i << "(parent = " << top.parent->joint_id << ")");
    SkelNode node;
    node.joint_id = int(i);
    node.joint_path = joints[i].str();
    node.joint_name = jointNames[i].str();
    node.bind_transform = bindTransforms[i];
    node.rest_transform = restTransforms[i];

    top.parent->children.emplace_back(std::move(node));

    // Push newly added child to process its children
    SkelNode *childPtr = &top.parent->children.back();
    stack.push_back({childPtr, 0});
  }

  return true;
}

}  // namespace detail

bool BuildSkelHierarchy(const Skeleton &skel, SkelNode &dst, std::string *err) {
  if (!skel.joints.authored()) {
    PUSH_ERROR_AND_RETURN(fmt::format(
        "Skeleton.joints attribute is not authored: {}", skel.name));
  }

  std::vector<value::token> joints;
  if (!skel.joints.get_value(&joints)) {
    PUSH_ERROR_AND_RETURN(
        fmt::format("Failed to get Skeleton.joints attribute: {}", skel.name));
  }

  if (joints.empty()) {
    PUSH_ERROR_AND_RETURN(
        fmt::format("Skeleton.joints attribute is empty: {}", skel.name));
  }

  std::vector<value::token> jointNames;

  if (skel.jointNames.authored()) {
    if (!skel.jointNames.get_value(&jointNames)) {
      PUSH_ERROR_AND_RETURN(fmt::format(
          "Failed to get Skeleton.jointNames attribute: {}", skel.name));
    }

    if (joints.size() != jointNames.size()) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("Skeleton.joints.size {} must be equal to "
                      "Skeleton.jointNames.size {}: {}",
                      joints.size(), jointNames.size(), skel.name));
    }
  } else {
    // Use joints
    jointNames.resize(joints.size());
    for (size_t i = 0; i < joints.size(); i++) {
      jointNames[i] = joints[i];
    }
  }


  // Track whether restTransforms is authored (for fallback computation later)
  bool restTransformsAuthored = skel.restTransforms.authored();
  bool bindTransformsAuthored = skel.bindTransforms.authored();

  // Read bindTransforms first (needed for potential restTransforms fallback)
  std::vector<value::matrix4d> bindTransforms;
  if (bindTransformsAuthored) {
    DCOUT("bindTransforms is authored");
    if (!skel.bindTransforms.get_value(&bindTransforms)) {
      PUSH_ERROR_AND_RETURN(fmt::format(
          "Failed to get Skeleton.bindTransforms attribute: {}", skel.name));
    }
    DCOUT("bindTransforms.size() = " << bindTransforms.size());
    if (bindTransforms.size() > 0) {
      DCOUT("bindTransforms[0] = " << bindTransforms[0]);
    } else {
      DCOUT("bindTransforms is authored but empty - using identity");
      bindTransformsAuthored = false;
      bindTransforms.assign(joints.size(), value::matrix4d::identity());
    }
  } else {
    DCOUT("bindTransforms is NOT authored - using identity");
    // Use identity when bindTransforms is not authored
    bindTransforms.assign(joints.size(), value::matrix4d::identity());
  }

  if (joints.size() != bindTransforms.size()) {
    PUSH_ERROR_AND_RETURN(
        fmt::format("Skeleton.joints.size {} must be equal to "
                    "Skeleton.bindTransforms.size {}: {}",
                    joints.size(), bindTransforms.size(), skel.name));
  }

  std::vector<value::matrix4d> restTransforms;
  if (restTransformsAuthored) {
    DCOUT("restTransforms is authored");
    if (!skel.restTransforms.get_value(&restTransforms)) {
      PUSH_ERROR_AND_RETURN(fmt::format(
          "Failed to get Skeleton.restTransforms attribute: {}", skel.name));
    }
    DCOUT("restTransforms.size() = " << restTransforms.size());
    if (restTransforms.size() > 0) {
      DCOUT("restTransforms[0] = " << restTransforms[0]);
    } else {
      DCOUT("restTransforms is authored but empty - using fallback");
      restTransformsAuthored = false;
    }
  } else if (bindTransformsAuthored) {
    // Fallback: compute restTransforms (local) from bindTransforms (world)
    // restTransform[i] = inverse(bindTransform[parent[i]]) * bindTransform[i]
    // For root joints (no parent), restTransform = bindTransform
    DCOUT("restTransforms is NOT authored - computing from bindTransforms");
  } else {
    DCOUT("restTransforms is NOT authored - using identity");
    // Neither authored: use identity matrices
    restTransforms.assign(joints.size(), value::matrix4d::identity());
  }

  // Build topology once (used for both restTransforms fallback and hierarchy construction)
  std::vector<int> parentJointIds;
  if (!BuildSkelTopology(joints, parentJointIds, err)) {
    return false;
  }

  // Compute restTransforms from bindTransforms if needed (uses parentJointIds built above)
  if (!restTransformsAuthored && bindTransformsAuthored) {
    restTransforms.resize(joints.size());
    for (size_t i = 0; i < joints.size(); i++) {
      int parentIdx = parentJointIds[i];
      if (parentIdx < 0) {
        // Root joint: use bindTransform directly (world space becomes local space)
        restTransforms[i] = bindTransforms[i];
      } else {
        // Child joint: compute local transform from world transforms.
        // Row-vector convention: childWorld = childLocal * parentWorld.
        value::matrix4d parentInverse;
        if (!inverse(bindTransforms[size_t(parentIdx)], parentInverse)) {
          DCOUT("Failed to compute inverse of parent bindTransform, using identity for restTransform");
          restTransforms[i] = value::matrix4d::identity();
        } else {
          restTransforms[i] = bindTransforms[i] * parentInverse;
        }
      }
    }
    DCOUT("Computed restTransforms from bindTransforms");
  } else if (!restTransformsAuthored) {
    restTransforms.assign(joints.size(), value::matrix4d::identity());
  }

  if (joints.size() != restTransforms.size()) {
    PUSH_ERROR_AND_RETURN(
        fmt::format("Skeleton.joints.size {} must be equal to "
                    "Skeleton.restTransforms.size {}: {}",
                    joints.size(), restTransforms.size(), skel.name));
  }

  // Just in case. Chek if topology is single-rooted.
  auto nroots = std::count_if(parentJointIds.begin(), parentJointIds.end(),
                              [](int x) { return x == -1; });

  if (nroots == 0) {
    PUSH_ERROR_AND_RETURN(fmt::format(
        "Invalid Skel topology. No root joint found: {}", skel.name));
  }

  if (nroots != 1) {
    PUSH_ERROR_AND_RETURN(
        fmt::format("Invalid Skel topology. Topology must be single-rooted, "
                    "but it has {} roots: {}",
                    nroots, skel.name));
  }

  // Build parent -> children map for O(n) hierarchy construction
  std::vector<std::vector<size_t>> childrenMap(joints.size());
  size_t rootIdx = 0;
  for (size_t i = 0; i < parentJointIds.size(); i++) {
    int parentId = parentJointIds[i];
    if (parentId < 0) {
      rootIdx = i;
    } else {
      childrenMap[size_t(parentId)].push_back(i);
    }
  }

  SkelNode root;
  root.joint_name = jointNames[rootIdx].str();
  root.joint_path = joints[rootIdx].str();
  root.joint_id = int(rootIdx);
  root.bind_transform = bindTransforms[rootIdx];
  root.rest_transform = restTransforms[rootIdx];

  DCOUT("parentJointIds = " << parentJointIds);

  // Construct hierarchy from children map.
  if (!detail::BuildSkelHierarchyImpl(root, childrenMap, joints, jointNames,
                                      bindTransforms, restTransforms,
                                      err)) {
    return false;
  }

  dst = root;

  return true;
}

namespace {

size_t CountSkelNodesIterative(const SkelNode &root) {
  size_t count = 0;
  StackVector<const SkelNode *, 4> stack;
  stack.reserve(64);
  stack.emplace_back(&root);

  while (!stack.empty()) {
    const SkelNode *node = stack.back();
    stack.pop_back();
    ++count;
    for (const auto &child : node->children) {
      stack.emplace_back(&child);
    }
  }

  return count;
}

// Iterative version of BuildSkelNameToIndexMap using explicit stack
void BuildSkelNameToIndexMapIterative(const SkelNode &root,
                                      SkelNameToIndexMap &m,
                                      size_t max_iter = kMaxDefaultTraversalLimit) {
  // Stack for DFS traversal
  StackVector<std::pair<const SkelNode *, size_t>, 4> stack;
  stack.reserve(64);
  stack.emplace_back(&root, 0);

  size_t iter = 0;
  while (!stack.empty()) {
    if (iter++ >= max_iter) break;
    std::pair<const SkelNode *, size_t> &entry = stack.back();
    const SkelNode *node = entry.first;
    size_t &child_idx = entry.second;

    // Process current node on first visit (child_idx == 0)
    if (child_idx == 0) {
      if (node->joint_id >= 0) {
        auto add_key = [&](const std::string &key) {
          if (key.empty()) {
            return;
          }
          // Keep the first authored mapping if duplicates appear.
          m.emplace(key, node->joint_id);
        };

        add_key(node->joint_name);
        add_key(node->joint_path);

        // Also register absolute/relative variants to handle mixed token forms
        // (e.g. "root/hip" vs "/root/hip") between Skeleton and SkelAnimation data.
        if (!node->joint_path.empty()) {
          if (node->joint_path[0] == '/') {
            add_key(node->joint_path.substr(1));
          } else {
            add_key("/" + node->joint_path);
          }
        }
      }
    }

    // Process children
    if (child_idx < node->children.size()) {
      size_t idx = child_idx++;
      stack.emplace_back(&node->children[idx], 0);
    } else {
      stack.pop_back();
    }
  }
}

} // namespace

SkelNameToIndexMap BuildSkelNameToIndexMap(const SkelHierarchy &skel) {

  SkelNameToIndexMap m;
  m.reserve(CountSkelNodesIterative(skel.root_node) * 3);

  BuildSkelNameToIndexMapIterative(skel.root_node, m);

  return m;
}

//
// Skeletal mesh extent computation
//

bool ComputeJointsExtent(
    const std::vector<value::matrix4d> &jointXforms,
    Extent *extent,
    float padding,
    const value::matrix4d *rootXform) {

  if (!extent) {
    return false;
  }

  if (jointXforms.empty()) {
    return false;
  }

  Extent e;  // initialized to +inf/-inf

  for (const auto &xf : jointXforms) {
    // Extract translation (pivot) from joint transform.
    // Row-major layout: translation is in row 3.
    value::float3 pivot;
    pivot[0] = float(xf.m[3][0]);
    pivot[1] = float(xf.m[3][1]);
    pivot[2] = float(xf.m[3][2]);

    if (rootXform) {
      // Transform pivot through rootXform: pivot * rootXform
      double px = double(pivot[0]);
      double py = double(pivot[1]);
      double pz = double(pivot[2]);

      double rx = px * rootXform->m[0][0] + py * rootXform->m[1][0] + pz * rootXform->m[2][0] + rootXform->m[3][0];
      double ry = px * rootXform->m[0][1] + py * rootXform->m[1][1] + pz * rootXform->m[2][1] + rootXform->m[3][1];
      double rz = px * rootXform->m[0][2] + py * rootXform->m[1][2] + pz * rootXform->m[2][2] + rootXform->m[3][2];
      double rw = px * rootXform->m[0][3] + py * rootXform->m[1][3] + pz * rootXform->m[2][3] + rootXform->m[3][3];

      if (std::abs(rw) > 1e-10) {
        rx /= rw;
        ry /= rw;
        rz /= rw;
      }

      pivot[0] = float(rx);
      pivot[1] = float(ry);
      pivot[2] = float(rz);
    }

    e.union_with(pivot);
  }

  if (padding > 0.0f) {
    e.lower[0] -= padding;
    e.lower[1] -= padding;
    e.lower[2] -= padding;
    e.upper[0] += padding;
    e.upper[1] += padding;
    e.upper[2] += padding;
  }

  *extent = e;
  return true;
}

float ComputeSkinnedExtentPadding(
    const std::vector<value::matrix4d> &restJointXforms,
    const Extent &meshRestExtent,
    const value::matrix4d &geomBindTransform) {

  if (restJointXforms.empty() || !meshRestExtent.is_valid()) {
    return 0.0f;
  }

  // Compute pivot extent from rest-pose joints
  Extent jointExtent;
  if (!ComputeJointsExtent(restJointXforms, &jointExtent)) {
    return 0.0f;
  }

  // Transform mesh rest extent corners by geomBindTransform
  // We need the 8 corners of the AABB transformed, then compute the new AABB
  const value::float3 &lo = meshRestExtent.lower;
  const value::float3 &hi = meshRestExtent.upper;

  Extent transformedMeshExtent;

  for (int i = 0; i < 8; i++) {
    float cx = (i & 1) ? hi[0] : lo[0];
    float cy = (i & 2) ? hi[1] : lo[1];
    float cz = (i & 4) ? hi[2] : lo[2];

    double px = double(cx);
    double py = double(cy);
    double pz = double(cz);

    // point * matrix (row-major, row-vector convention)
    double rx = px * geomBindTransform.m[0][0] + py * geomBindTransform.m[1][0] + pz * geomBindTransform.m[2][0] + geomBindTransform.m[3][0];
    double ry = px * geomBindTransform.m[0][1] + py * geomBindTransform.m[1][1] + pz * geomBindTransform.m[2][1] + geomBindTransform.m[3][1];
    double rz = px * geomBindTransform.m[0][2] + py * geomBindTransform.m[1][2] + pz * geomBindTransform.m[2][2] + geomBindTransform.m[3][2];
    double rw = px * geomBindTransform.m[0][3] + py * geomBindTransform.m[1][3] + pz * geomBindTransform.m[2][3] + geomBindTransform.m[3][3];

    if (std::abs(rw) > 1e-10) {
      rx /= rw;
      ry /= rw;
      rz /= rw;
    }

    value::float3 tp;
    tp[0] = float(rx);
    tp[1] = float(ry);
    tp[2] = float(rz);
    transformedMeshExtent.union_with(tp);
  }

  // Padding = max distance that the mesh extent exceeds the joint extent
  // on any axis in any direction
  float padding = 0.0f;

  for (size_t i = 0; i < 3; i++) {
    float diffLo = jointExtent.lower[i] - transformedMeshExtent.lower[i];
    float diffHi = transformedMeshExtent.upper[i] - jointExtent.upper[i];

    padding = (std::max)(padding, (std::max)(diffLo, 0.0f));
    padding = (std::max)(padding, (std::max)(diffHi, 0.0f));
  }

  return padding;
}

bool SkinPointsLBS(
    const std::vector<value::point3f> &restPoints,
    const value::matrix4d &geomBindTransform,
    const std::vector<value::matrix4d> &jointXforms,
    const std::vector<int> &jointIndices,
    const std::vector<float> &jointWeights,
    int numInfluencesPerPoint,
    std::vector<value::point3f> *skinnedPoints,
    std::string *err) {

  if (!skinnedPoints) {
    if (err) { *err = "skinnedPoints is null."; }
    return false;
  }

  if (numInfluencesPerPoint < 1) {
    if (err) { *err = "numInfluencesPerPoint must be >= 1."; }
    return false;
  }

  size_t numPoints = restPoints.size();
  size_t expectedSize = numPoints * size_t(numInfluencesPerPoint);

  if (jointIndices.size() != expectedSize) {
    if (err) {
      *err = "jointIndices size mismatch: expected " +
             std::to_string(expectedSize) + ", got " +
             std::to_string(jointIndices.size()) + ".";
    }
    return false;
  }

  if (jointWeights.size() != expectedSize) {
    if (err) {
      *err = "jointWeights size mismatch: expected " +
             std::to_string(expectedSize) + ", got " +
             std::to_string(jointWeights.size()) + ".";
    }
    return false;
  }

  int numJoints = int(jointXforms.size());

  skinnedPoints->resize(numPoints);
  value::matrix4d inverseGeomBindTransform = value::matrix4d::identity();
  if (!inverse(geomBindTransform, inverseGeomBindTransform)) {
    if (err) { *err = "Failed to invert geomBindTransform."; }
    return false;
  }

  for (size_t pi = 0; pi < numPoints; pi++) {
    // Transform rest point into skeleton space via geomBindTransform
    const value::point3f &rp = restPoints[pi];
    double px = double(rp.x);
    double py = double(rp.y);
    double pz = double(rp.z);

    double sx = px * geomBindTransform.m[0][0] + py * geomBindTransform.m[1][0] + pz * geomBindTransform.m[2][0] + geomBindTransform.m[3][0];
    double sy = px * geomBindTransform.m[0][1] + py * geomBindTransform.m[1][1] + pz * geomBindTransform.m[2][1] + geomBindTransform.m[3][1];
    double sz = px * geomBindTransform.m[0][2] + py * geomBindTransform.m[1][2] + pz * geomBindTransform.m[2][2] + geomBindTransform.m[3][2];
    double sw = px * geomBindTransform.m[0][3] + py * geomBindTransform.m[1][3] + pz * geomBindTransform.m[2][3] + geomBindTransform.m[3][3];

    if (std::abs(sw) > 1e-10) {
      sx /= sw;
      sy /= sw;
      sz /= sw;
    }

    // Accumulate weighted joint transforms
    double outx = 0.0, outy = 0.0, outz = 0.0;

    size_t base = pi * size_t(numInfluencesPerPoint);
    for (int ji = 0; ji < numInfluencesPerPoint; ji++) {
      int idx = jointIndices[base + size_t(ji)];
      float w = jointWeights[base + size_t(ji)];

      if (w == 0.0f || idx < 0 || idx >= numJoints) {
        continue;
      }

      const value::matrix4d &jx = jointXforms[size_t(idx)];

      // skelPoint * jointXform
      double tx = sx * jx.m[0][0] + sy * jx.m[1][0] + sz * jx.m[2][0] + jx.m[3][0];
      double ty = sx * jx.m[0][1] + sy * jx.m[1][1] + sz * jx.m[2][1] + jx.m[3][1];
      double tz = sx * jx.m[0][2] + sy * jx.m[1][2] + sz * jx.m[2][2] + jx.m[3][2];

      outx += double(w) * tx;
      outy += double(w) * ty;
      outz += double(w) * tz;
    }

    double gx = outx * inverseGeomBindTransform.m[0][0] +
                outy * inverseGeomBindTransform.m[1][0] +
                outz * inverseGeomBindTransform.m[2][0] +
                inverseGeomBindTransform.m[3][0];
    double gy = outx * inverseGeomBindTransform.m[0][1] +
                outy * inverseGeomBindTransform.m[1][1] +
                outz * inverseGeomBindTransform.m[2][1] +
                inverseGeomBindTransform.m[3][1];
    double gz = outx * inverseGeomBindTransform.m[0][2] +
                outy * inverseGeomBindTransform.m[1][2] +
                outz * inverseGeomBindTransform.m[2][2] +
                inverseGeomBindTransform.m[3][2];
    double gw = outx * inverseGeomBindTransform.m[0][3] +
                outy * inverseGeomBindTransform.m[1][3] +
                outz * inverseGeomBindTransform.m[2][3] +
                inverseGeomBindTransform.m[3][3];
    if (std::abs(gw) > 1e-10) {
      gx /= gw;
      gy /= gw;
      gz /= gw;
    }

    (*skinnedPoints)[pi].x = float(gx);
    (*skinnedPoints)[pi].y = float(gy);
    (*skinnedPoints)[pi].z = float(gz);
  }

  return true;
}

bool ComputeSkinnedMeshExtent(
    const std::vector<value::matrix4d> &jointXforms,
    const std::vector<value::matrix4d> &restJointXforms,
    const Extent &meshRestExtent,
    const value::matrix4d &geomBindTransform,
    Extent *extent,
    const value::matrix4d *rootXform) {

  if (!extent) {
    return false;
  }

  float padding = ComputeSkinnedExtentPadding(
      restJointXforms, meshRestExtent, geomBindTransform);

  return ComputeJointsExtent(jointXforms, extent, padding, rootXform);
}

//
// Skeleton transform utilities (ported from OpenUSD UsdSkelUtils)
//

bool ConcatJointTransforms(
    const std::vector<int> &topology,
    const std::vector<value::matrix4d> &localXforms,
    std::vector<value::matrix4d> *worldXforms,
    const value::matrix4d *rootXform) {

  if (!worldXforms) {
    return false;
  }

  size_t numJoints = topology.size();
  if (localXforms.size() != numJoints) {
    return false;
  }

  worldXforms->resize(numJoints);

  // Topology guarantees parent index < child index, so a single forward pass
  // computes all world-space transforms.
  for (size_t i = 0; i < numJoints; i++) {
    int parent = topology[i];
    if (parent < 0) {
      // Root joint
      if (rootXform) {
        (*worldXforms)[i] = localXforms[i] * (*rootXform);
      } else {
        (*worldXforms)[i] = localXforms[i];
      }
    } else if (size_t(parent) < numJoints) {
      (*worldXforms)[i] = localXforms[i] * (*worldXforms)[size_t(parent)];
    } else {
      // Invalid parent - treat as root
      (*worldXforms)[i] = localXforms[i];
    }
  }

  return true;
}

bool ComputeJointLocalTransforms(
    const std::vector<int> &topology,
    const std::vector<value::matrix4d> &worldXforms,
    std::vector<value::matrix4d> *localXforms,
    const value::matrix4d *inverseRootXform) {

  if (!localXforms) {
    return false;
  }

  size_t numJoints = topology.size();
  if (worldXforms.size() != numJoints) {
    return false;
  }

  localXforms->resize(numJoints);

  for (size_t i = 0; i < numJoints; i++) {
    int parent = topology[i];
    if (parent < 0) {
      // Root joint
      if (inverseRootXform) {
        (*localXforms)[i] = worldXforms[i] * (*inverseRootXform);
      } else {
        (*localXforms)[i] = worldXforms[i];
      }
    } else if (size_t(parent) < numJoints) {
      value::matrix4d parentInv = inverse(worldXforms[size_t(parent)]);
      (*localXforms)[i] = worldXforms[i] * parentInv;
    } else {
      (*localXforms)[i] = worldXforms[i];
    }
  }

  return true;
}

value::matrix4d SkelMakeTransform(
    const value::float3 &translation,
    const value::quatf &rotation,
    const value::half3 &scale) {

  // Build rotation matrix from quaternion
  value::matrix4d rotMat = to_matrix(rotation);

  // Apply scale to the rotation matrix (upper-left 3x3)
  double sx = double(half_to_float(scale[0]));
  double sy = double(half_to_float(scale[1]));
  double sz = double(half_to_float(scale[2]));

  rotMat.m[0][0] *= sx; rotMat.m[0][1] *= sx; rotMat.m[0][2] *= sx;
  rotMat.m[1][0] *= sy; rotMat.m[1][1] *= sy; rotMat.m[1][2] *= sy;
  rotMat.m[2][0] *= sz; rotMat.m[2][1] *= sz; rotMat.m[2][2] *= sz;

  // Set translation
  rotMat.m[3][0] = double(translation[0]);
  rotMat.m[3][1] = double(translation[1]);
  rotMat.m[3][2] = double(translation[2]);

  return rotMat;
}

bool SkinNormalsLBS(
    const std::vector<value::normal3f> &restNormals,
    const value::matrix4d &geomBindTransform,
    const std::vector<value::matrix4d> &jointXforms,
    const std::vector<int> &jointIndices,
    const std::vector<float> &jointWeights,
    int numInfluencesPerPoint,
    std::vector<value::normal3f> *skinnedNormals,
    std::string *err) {

  if (!skinnedNormals) {
    if (err) { *err = "skinnedNormals is null."; }
    return false;
  }

  if (numInfluencesPerPoint < 1) {
    if (err) { *err = "numInfluencesPerPoint must be >= 1."; }
    return false;
  }

  size_t numPoints = restNormals.size();
  size_t expectedSize = numPoints * size_t(numInfluencesPerPoint);

  if (jointIndices.size() != expectedSize) {
    if (err) {
      *err = "jointIndices size mismatch: expected " +
             std::to_string(expectedSize) + ", got " +
             std::to_string(jointIndices.size()) + ".";
    }
    return false;
  }

  if (jointWeights.size() != expectedSize) {
    if (err) {
      *err = "jointWeights size mismatch: expected " +
             std::to_string(expectedSize) + ", got " +
             std::to_string(jointWeights.size()) + ".";
    }
    return false;
  }

  int numJoints = int(jointXforms.size());

  skinnedNormals->resize(numPoints);
  value::matrix4d inverseGeomBindTransform = value::matrix4d::identity();
  if (!inverse(geomBindTransform, inverseGeomBindTransform)) {
    if (err) { *err = "Failed to invert geomBindTransform."; }
    return false;
  }

  // For normals, we use inverse-transpose of the skinning matrix.
  // Since we accumulate the weighted skinning matrix per vertex first,
  // we can compute its inverse-transpose at the end. But for LBS where
  // weights sum to 1, we can skin normals with the upper-left 3x3
  // (direction only, no translation) and then renormalize.

  for (size_t pi = 0; pi < numPoints; pi++) {
    const value::normal3f &rn = restNormals[pi];
    double nx = double(rn[0]);
    double ny = double(rn[1]);
    double nz = double(rn[2]);

    // Transform normal into skeleton space via geomBindTransform (direction only)
    double snx = nx * geomBindTransform.m[0][0] + ny * geomBindTransform.m[1][0] + nz * geomBindTransform.m[2][0];
    double sny = nx * geomBindTransform.m[0][1] + ny * geomBindTransform.m[1][1] + nz * geomBindTransform.m[2][1];
    double snz = nx * geomBindTransform.m[0][2] + ny * geomBindTransform.m[1][2] + nz * geomBindTransform.m[2][2];

    // Accumulate weighted joint transforms (direction only)
    double outx = 0.0, outy = 0.0, outz = 0.0;

    size_t base = pi * size_t(numInfluencesPerPoint);
    for (int ji = 0; ji < numInfluencesPerPoint; ji++) {
      int idx = jointIndices[base + size_t(ji)];
      float w = jointWeights[base + size_t(ji)];

      if (w == 0.0f || idx < 0 || idx >= numJoints) {
        continue;
      }

      const value::matrix4d &jx = jointXforms[size_t(idx)];

      // skelNormal * jointXform (3x3 only for directions)
      double tx = snx * jx.m[0][0] + sny * jx.m[1][0] + snz * jx.m[2][0];
      double ty = snx * jx.m[0][1] + sny * jx.m[1][1] + snz * jx.m[2][1];
      double tz = snx * jx.m[0][2] + sny * jx.m[1][2] + snz * jx.m[2][2];

      outx += double(w) * tx;
      outy += double(w) * ty;
      outz += double(w) * tz;
    }

    double gx = outx * inverseGeomBindTransform.m[0][0] +
                outy * inverseGeomBindTransform.m[1][0] +
                outz * inverseGeomBindTransform.m[2][0];
    double gy = outx * inverseGeomBindTransform.m[0][1] +
                outy * inverseGeomBindTransform.m[1][1] +
                outz * inverseGeomBindTransform.m[2][1];
    double gz = outx * inverseGeomBindTransform.m[0][2] +
                outy * inverseGeomBindTransform.m[1][2] +
                outz * inverseGeomBindTransform.m[2][2];

    // Renormalize
    outx = gx;
    outy = gy;
    outz = gz;
    double len = std::sqrt(outx * outx + outy * outy + outz * outz);
    if (len > 1e-10) {
      outx /= len;
      outy /= len;
      outz /= len;
    }

    (*skinnedNormals)[pi][0] = float(outx);
    (*skinnedNormals)[pi][1] = float(outy);
    (*skinnedNormals)[pi][2] = float(outz);
  }

  return true;
}

bool ExpandConstantInfluencesToVarying(
    const std::vector<int> &indices,
    const std::vector<float> &weights,
    size_t numVertices,
    std::vector<int> *expandedIndices,
    std::vector<float> *expandedWeights) {

  if (!expandedIndices || !expandedWeights) {
    return false;
  }

  if (indices.size() != weights.size()) {
    return false;
  }

  size_t numInfluences = indices.size();
  if (numInfluences == 0 || numVertices == 0) {
    expandedIndices->clear();
    expandedWeights->clear();
    return true;
  }

  size_t totalSize = numVertices * numInfluences;
  expandedIndices->resize(totalSize);
  expandedWeights->resize(totalSize);

  for (size_t v = 0; v < numVertices; v++) {
    size_t offset = v * numInfluences;
    for (size_t i = 0; i < numInfluences; i++) {
      (*expandedIndices)[offset + i] = indices[i];
      (*expandedWeights)[offset + i] = weights[i];
    }
  }

  return true;
}

}  // namespace tydra
}  // namespace lightusd

#undef PushError

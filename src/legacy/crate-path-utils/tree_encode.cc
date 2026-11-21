//
// Crate format PATHS tree encoding implementation
// SPDX-License-Identifier: Apache 2.0
//
#include "tree_encode.hh"
#include <algorithm>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>


namespace crate {

// ============================================================================
// Internal Tree Node Structure
// ============================================================================

/// Internal tree node (not exposed in public API)
struct PathTreeNode {
  std::string element_name;          // Element name (e.g., "World", "Geom")
  TokenIndex element_token_index;    // Token index for this element
  PathIndex path_index;              // Index into original paths vector
  bool is_property;                  // True if this is a property path element

  PathTreeNode* parent = nullptr;
  PathTreeNode* first_child = nullptr;
  PathTreeNode* next_sibling = nullptr;

  PathTreeNode(const std::string& name, TokenIndex token_idx, PathIndex path_idx, bool is_prop)
      : element_name(name), element_token_index(token_idx), path_index(path_idx), is_property(is_prop) {}
};

// ============================================================================
// TokenTable Implementation
// ============================================================================

TokenIndex TokenTable::GetOrCreateToken(const std::string& str, bool is_property) {
  auto it = tokens_.find(str);
  if (it != tokens_.end()) {
    return it->second;
  }

  TokenIndex index = next_index_++;

  // Properties use negative indices (as per OpenUSD convention)
  if (is_property) {
    index = -index - 1;  // -1, -2, -3, ...
  }

  tokens_[str] = index;
  reverse_tokens_[index] = str;

  return index;
}

std::string TokenTable::GetToken(TokenIndex index) const {
  auto it = reverse_tokens_.find(index);
  if (it == reverse_tokens_.end()) {
    return "";
  }
  return it->second;
}

void TokenTable::Clear() {
  tokens_.clear();
  reverse_tokens_.clear();
  next_index_ = 0;
}

// ============================================================================
// Tree Building
// ============================================================================

static std::unique_ptr<PathTreeNode> BuildPathTree(
  const std::vector<SimplePath>& sorted_paths,
  TokenTable& token_table
) {
  if (sorted_paths.empty()) {
    return nullptr;
  }

  // Create root node (represents the root "/" path)
  // Note: In Crate format, root is implicit and starts with empty element
  // CRITICAL: Root element must have a token in the token table
  TokenIndex root_token_idx = token_table.GetOrCreateToken("", false);
  auto root = std::make_unique<PathTreeNode>("", root_token_idx, 0, false);
  root->path_index = 0;  // Root path is always at index 0 if it exists

  // Map from path string to node (for quick lookup)
  std::map<std::string, PathTreeNode*> path_to_node;
  path_to_node["/"] = root.get();

  for (size_t path_idx = 0; path_idx < sorted_paths.size(); ++path_idx) {
    const SimplePath& path = sorted_paths[path_idx];

    // Parse prim part
    std::string prim_part = path.prim_part();
    std::string prop_part = path.prop_part();


    // Skip root path - it's already represented by root node
    if (prim_part == "/" && prop_part.empty()) {
      continue;
    }

    // Handle root with property (e.g., "/.prop")
    if (prim_part == "/" && !prop_part.empty()) {
      TokenIndex token_idx = token_table.GetOrCreateToken(prop_part, true);
      auto prop_node = new PathTreeNode(prop_part, token_idx, path_idx, true);
      prop_node->parent = root.get();

      if (root->first_child == nullptr) {
        root->first_child = prop_node;
      } else {
        PathTreeNode* sibling = root->first_child;
        while (sibling->next_sibling != nullptr) {
          sibling = sibling->next_sibling;
        }
        sibling->next_sibling = prop_node;
      }
      continue;
    }

    // Split prim part into elements
    // This handles both regular prims (/A/B/C) and variant paths (/A{varSet}{varSet=val}/B)
    std::vector<std::string> elements;
    std::string current_path;

    if (!prim_part.empty() && prim_part[0] == '/') {
      current_path = "/";
      size_t start = 1;

      while (start < prim_part.size()) {
        size_t end = prim_part.find('/', start);
        if (end == std::string::npos) {
          end = prim_part.size();
        }

        std::string segment = prim_part.substr(start, end - start);
        if (!segment.empty()) {
          // Check if segment contains variant selections (e.g., "Implicits{shapeVariant}")
          // Split into base prim name and variant parts
          size_t brace_pos = segment.find('{');
          if (brace_pos != std::string::npos) {
            // Extract base prim name (if any)
            if (brace_pos > 0) {
              std::string base_name = segment.substr(0, brace_pos);
              elements.push_back(base_name);
            }

            // Extract all variant selections (can be multiple like {a}{a=b})
            size_t var_start = brace_pos;
            while (var_start < segment.size() && segment[var_start] == '{') {
              size_t var_end = segment.find('}', var_start);
              if (var_end == std::string::npos) {
                // Malformed variant, just take the rest
                elements.push_back(segment.substr(var_start));
                break;
              }
              // Include the closing brace
              std::string variant_part = segment.substr(var_start, var_end - var_start + 1);
              elements.push_back(variant_part);
              var_start = var_end + 1;
            }
          } else {
            // No variant selection, just a regular prim name
            elements.push_back(segment);
          }
        }

        start = end + 1;
      }
    }

    // Build prim hierarchy
    PathTreeNode* parent_node = root.get();
    current_path = "";

    for (size_t i = 0; i < elements.size(); ++i) {
      const std::string& element = elements[i];
      // Variant elements (starting with '{') are appended directly without '/'
      bool is_variant = !element.empty() && element[0] == '{';
      if (current_path.empty()) {
        current_path = "/" + element;
      } else if (is_variant) {
        current_path = current_path + element;  // No '/' before variant selections
      } else {
        current_path = current_path + "/" + element;
      }

      // Check if node already exists
      auto it = path_to_node.find(current_path);
      if (it != path_to_node.end()) {
        parent_node = it->second;
        continue;
      }

      // Create new node
      TokenIndex token_idx = token_table.GetOrCreateToken(element, false);
      // Use UINT64_MAX as sentinel for intermediate nodes that don't have their own path index
      // This avoids conflicts with path_index=0 which is reserved for the root "/"
      PathIndex node_path_idx = (i == elements.size() - 1 && prop_part.empty()) ? path_idx : UINT64_MAX;

      auto new_node = new PathTreeNode(element, token_idx, node_path_idx, false);
      new_node->parent = parent_node;

      // Add as child to parent
      if (parent_node->first_child == nullptr) {
        parent_node->first_child = new_node;
      } else {
        // Find last sibling and append
        PathTreeNode* sibling = parent_node->first_child;
        while (sibling->next_sibling != nullptr) {
          sibling = sibling->next_sibling;
        }
        sibling->next_sibling = new_node;
      }

      path_to_node[current_path] = new_node;
      parent_node = new_node;
    }

    // Add property if present
    if (!prop_part.empty()) {
      TokenIndex token_idx = token_table.GetOrCreateToken(prop_part, true);
      auto prop_node = new PathTreeNode(prop_part, token_idx, path_idx, true);
      prop_node->parent = parent_node;

      if (parent_node->first_child == nullptr) {
        parent_node->first_child = prop_node;
      } else {
        PathTreeNode* sibling = parent_node->first_child;
        while (sibling->next_sibling != nullptr) {
          sibling = sibling->next_sibling;
        }
        sibling->next_sibling = prop_node;
      }
    }
  }

  return root;
}

// ============================================================================
// Tree Walking and Encoding
// ============================================================================

static int32_t CalculateJump(
  const PathTreeNode* /* node */,
  bool has_child,
  bool has_sibling,
  size_t sibling_offset
) {
  if (!has_child && !has_sibling) {
    return -2;  // Leaf node
  }

  if (has_child && !has_sibling) {
    return -1;  // Only child follows
  }

  if (!has_child && has_sibling) {
    return 0;   // Only sibling follows
  }

  // Both child and sibling exist
  // Return offset to sibling (positive value)
  return static_cast<int32_t>(sibling_offset);
}

static void WalkTreeDepthFirst(
  PathTreeNode* node,
  std::vector<PathIndex>& path_indexes,
  std::vector<TokenIndex>& element_token_indexes,
  std::vector<int32_t>& jumps,
  std::vector<size_t>& sibling_offsets,
  bool include_node = true  // Whether to include this node in output
) {
  if (node == nullptr) {
    return;
  }

  size_t current_pos = 0;
  bool has_child = (node->first_child != nullptr);
  bool has_sibling = (node->next_sibling != nullptr);

  if (include_node) {
    // Record current position
    current_pos = path_indexes.size();

    // Add this node
    path_indexes.push_back(node->path_index);
    element_token_indexes.push_back(node->element_token_index);

    // Placeholder for jump (will be filled in later if needed)
    jumps.push_back(0);

    // If we have both child and sibling, we need to track sibling offset
    if (has_child && has_sibling) {
      sibling_offsets.push_back(current_pos);  // Mark for later update
    }
  }

  // Process child first (depth-first)
  size_t sibling_pos = 0;
  if (has_child) {
    WalkTreeDepthFirst(node->first_child, path_indexes, element_token_indexes, jumps, sibling_offsets, true);

    // If we also have a sibling, record where it will be
    if (has_sibling && include_node) {
      sibling_pos = path_indexes.size();
    }
  }

  if (include_node) {
    // Calculate and set jump value
    size_t offset_to_sibling = has_sibling ? (sibling_pos - current_pos) : 0;
    jumps[current_pos] = CalculateJump(node, has_child, has_sibling, offset_to_sibling);
  }

  // Process sibling
  if (has_sibling) {
    WalkTreeDepthFirst(node->next_sibling, path_indexes, element_token_indexes, jumps, sibling_offsets, true);
  }
}

CompressedPathTree EncodePaths(const std::vector<SimplePath>& sorted_paths) {
  CompressedPathTree result;

  if (sorted_paths.empty()) {
    return result;
  }

  // Build tree structure
  auto root = BuildPathTree(sorted_paths, result.token_table);

  if (!root) {
    return result;
  }

  // Walk tree and generate arrays
  std::vector<size_t> sibling_offsets;

  // Start from root's children (root itself is implicit in the structure)
  // But we need to add root as the first node
  result.path_indexes.push_back(root->path_index);
  result.element_token_indexes.push_back(root->element_token_index);
  result.jumps.push_back(-1);  // Root always has children (or is a leaf if no children)

  if (root->first_child) {
    // Process children
    WalkTreeDepthFirst(root->first_child, result.path_indexes, result.element_token_indexes,
                       result.jumps, sibling_offsets, true);

    // Update root's jump value
    if (!root->first_child->next_sibling) {
      result.jumps[0] = -1;  // Only child
    } else {
      result.jumps[0] = -1;  // Child follows (siblings are also children of root)
    }
  } else {
    // No children - root is a leaf
    result.jumps[0] = -2;
  }

  // Clean up tree (delete nodes)
  std::function<void(PathTreeNode*)> delete_tree = [&](PathTreeNode* node) {
    if (!node) return;

    // Delete children
    PathTreeNode* child = node->first_child;
    while (child) {
      PathTreeNode* next = child->next_sibling;
      delete_tree(child);
      delete child;
      child = next;
    }
  };

  delete_tree(root.get());

  return result;
}

// ============================================================================
// Tree Decoding
// ============================================================================

std::vector<SimplePath> DecodePaths(const CompressedPathTree& compressed) {
  if (compressed.empty()) {
    return {};
  }

  // Create a map from path_index to reconstructed path
  std::map<PathIndex, SimplePath> path_map;

  // Recursive decoder
  std::function<void(size_t, std::string)> decode_recursive;
  decode_recursive = [&](size_t idx, std::string current_prim) {
    if (idx >= compressed.size()) {
      return;
    }

    PathIndex path_idx = compressed.path_indexes[idx];
    TokenIndex token_idx = compressed.element_token_indexes[idx];
    int32_t jump = compressed.jumps[idx];

    // Get element name
    std::string element = compressed.token_table.GetToken(token_idx);
    bool is_property = (token_idx < 0);

    // Build current path
    std::string prim_part = current_prim;
    std::string prop_part;

    if (is_property) {
      // Property path - prim_part stays the same, prop_part is the element
      prop_part = element;
    } else {
      // Prim path - build new prim path
      if (element.empty()) {
        // Root node
        prim_part = "/";
      } else if (current_prim == "/") {
        prim_part = "/" + element;
      } else if (current_prim.empty()) {
        prim_part = "/" + element;
      } else {
        prim_part = current_prim + "/" + element;
      }
    }

    // Store path if this node represents an actual path (not just a tree structure node)
    // Nodes with path_index > 0 or the root (path_idx==0 and element.empty()) are actual paths
    if (path_idx > 0 || (path_idx == 0 && element.empty())) {
      path_map[path_idx] = SimplePath(prim_part, prop_part);
    }

    // Process according to jump value
    if (jump == -2) {
      // Leaf - done
      return;
    } else if (jump == -1) {
      // Only child
      // For prim nodes, child inherits the prim path
      // For property nodes, this shouldn't happen (properties are leaves)
      if (!is_property) {
        decode_recursive(idx + 1, prim_part);
      }
    } else if (jump == 0) {
      // Only sibling
      // Sibling has the same parent, so use current_prim
      decode_recursive(idx + 1, current_prim);
    } else if (jump > 0) {
      // Both child and sibling
      // Child is next
      if (!is_property) {
        decode_recursive(idx + 1, prim_part);
      } else {
        decode_recursive(idx + 1, current_prim);
      }
      // Sibling is at offset (same parent)
      decode_recursive(idx + static_cast<size_t>(jump), current_prim);
    }
  };

  // Start decoding from root (index 0)
  // Root starts with empty path
  decode_recursive(0, "");

  // Convert map to vector (sorted by path_index)
  std::vector<SimplePath> result;
  for (const auto& pair : path_map) {
    result.push_back(pair.second);
  }

  return result;
}

// ============================================================================
// Validation
// ============================================================================

bool ValidateRoundTrip(
  const std::vector<SimplePath>& original,
  const CompressedPathTree& compressed,
  std::vector<std::string>* errors
) {
  std::vector<SimplePath> decoded = DecodePaths(compressed);

  if (original.size() != decoded.size()) {
    if (errors) {
      errors->push_back("Size mismatch: original=" + std::to_string(original.size()) +
                       ", decoded=" + std::to_string(decoded.size()));
    }
    return false;
  }

  bool success = true;
  for (size_t i = 0; i < original.size(); ++i) {
    if (original[i].GetString() != decoded[i].GetString()) {
      success = false;
      if (errors) {
        errors->push_back("Path [" + std::to_string(i) + "] mismatch: " +
                         "original=\"" + original[i].GetString() + "\", " +
                         "decoded=\"" + decoded[i].GetString() + "\"");
      }
    }
  }

  return success;
}


} // namespace crate

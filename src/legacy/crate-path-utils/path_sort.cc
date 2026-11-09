//
// Path sorting implementation
// SPDX-License-Identifier: Apache 2.0
//
#include "path_sort.hh"
#include <algorithm>
#include <cstdint>
#include <vector>
#include <string>

namespace crate {

// (PathElement now declared in path_sort.hh for precomputed sort keys.)

static std::vector<PathElement> ParsePath(const std::string& prim_part, const std::string& prop_part) {
  std::vector<PathElement> elements;

  bool is_absolute = !prim_part.empty() && prim_part[0] == '/';

  // Parse prim part
  if (!prim_part.empty()) {
    if (prim_part == "/") {
      elements.push_back(PathElement("", is_absolute, false, 0));
      return elements;
    }

    size_t start = is_absolute ? 1 : 0;
    int depth = 0;

    while (start < prim_part.size()) {
      size_t end = prim_part.find('/', start);
      if (end == std::string::npos) {
        end = prim_part.size();
      }

      std::string element = prim_part.substr(start, end - start);
      if (!element.empty()) {
        depth++;
        elements.push_back(PathElement(element, is_absolute, false, depth));
      }

      start = end + 1;
    }
  }

  // Parse property part
  if (!prop_part.empty()) {
    int depth = static_cast<int>(elements.size()) + 1;
    elements.push_back(PathElement(prop_part, is_absolute, true, depth));
  }

  return elements;
}

static int CompareElements(const PathElement& lhs, const PathElement& rhs) {
  if (lhs.name < rhs.name) {
    return -1;
  } else if (lhs.name > rhs.name) {
    return 1;
  }
  return 0;
}

static int ComparePathElements(
  const std::vector<PathElement>& lhs_elements,
  const std::vector<PathElement>& rhs_elements
) {
  int lhs_count = static_cast<int>(lhs_elements.size());
  int rhs_count = static_cast<int>(rhs_elements.size());

  // Root node handling
  if (lhs_count == 0 || rhs_count == 0) {
    if (lhs_count == 0 && rhs_count > 0) {
      return -1;
    } else if (lhs_count > 0 && rhs_count == 0) {
      return 1;
    }
    return 0;
  }

  int diff = rhs_count - lhs_count;
  int lhs_idx = lhs_count - 1;
  int rhs_idx = rhs_count - 1;

  // Walk to same depth
  while (diff < 0) {
    lhs_idx--;
    diff++;
  }
  while (diff > 0) {
    rhs_idx--;
    diff--;
  }

  // Check if same path up to this point
  bool same_prefix = true;
  if (lhs_idx >= 0 && rhs_idx >= 0) {
    int l = lhs_idx;
    int r = rhs_idx;
    while (l >= 0 && r >= 0) {
      if (lhs_elements[static_cast<size_t>(l)].name != rhs_elements[static_cast<size_t>(r)].name ||
          lhs_elements[static_cast<size_t>(l)].is_property != rhs_elements[static_cast<size_t>(r)].is_property) {
        same_prefix = false;
        break;
      }
      l--;
      r--;
    }
  }

  if (same_prefix && lhs_idx >= 0 && rhs_idx >= 0) {
    // Differ only in tail - shorter is less
    if (lhs_count < rhs_count) {
      return -1;
    } else if (lhs_count > rhs_count) {
      return 1;
    }
    return 0;
  }

  // Find first differing elements with same parent
  lhs_idx = lhs_count - 1;
  rhs_idx = rhs_count - 1;

  diff = rhs_count - lhs_count;
  while (diff < 0) {
    lhs_idx--;
    diff++;
  }
  while (diff > 0) {
    rhs_idx--;
    diff--;
  }

  // Walk up both until parents match
  while (lhs_idx > 0 && rhs_idx > 0) {
    bool parents_match = true;
    if (lhs_idx > 0 && rhs_idx > 0) {
      for (int i = 0; i < lhs_idx && i < rhs_idx; i++) {
        if (lhs_elements[static_cast<size_t>(i)].name != rhs_elements[static_cast<size_t>(i)].name ||
            lhs_elements[static_cast<size_t>(i)].is_property != rhs_elements[static_cast<size_t>(i)].is_property) {
          parents_match = false;
          break;
        }
      }
    }

    if (parents_match) {
      break;
    }

    lhs_idx--;
    rhs_idx--;
  }

  // Compare elements at divergence point
  if (lhs_idx >= 0 && rhs_idx >= 0 &&
      lhs_idx < lhs_count && rhs_idx < rhs_count) {
    return CompareElements(lhs_elements[static_cast<size_t>(lhs_idx)], rhs_elements[static_cast<size_t>(rhs_idx)]);
  }

  // Fallback
  if (lhs_count < rhs_count) {
    return -1;
  } else if (lhs_count > rhs_count) {
    return 1;
  }

  return 0;
}

ParsedSimplePath MakeParsedSimplePath(const std::string& prim_part,
                                      const std::string& prop_part) {
  ParsedSimplePath parsed;
  // IMPORTANT: Parse prim parts ONLY for prim comparison
  // Properties should not be included in prim path comparison,
  // as they don't add depth to the path hierarchy.
  // In USD path ordering: /A < /A.prop < /A/B
  // (prim before its properties, properties before children)
  parsed.prim_elements = ParsePath(prim_part, "");  // No property
  parsed.is_absolute = !prim_part.empty() && (prim_part[0] == '/');
  parsed.prop = prop_part;
  return parsed;
}

int CompareParsedPaths(const ParsedSimplePath& lhs,
                       const ParsedSimplePath& rhs) {
  // Absolute paths are less than relative paths
  if (lhs.is_absolute != rhs.is_absolute) {
    return lhs.is_absolute ? -1 : 1;
  }

  // Compare prim parts ONLY
  int prim_cmp = ComparePathElements(lhs.prim_elements, rhs.prim_elements);
  if (prim_cmp != 0) {
    return prim_cmp;
  }

  // Prim parts equal, compare property parts
  // Properties sort after their prim but before any child prims
  if (lhs.prop.empty() && rhs.prop.empty()) {
    return 0;
  }

  // Prim without property comes first
  if (lhs.prop.empty()) {
    return -1;
  }

  if (rhs.prop.empty()) {
    return 1;
  }

  // Both have properties - compare alphabetically
  if (lhs.prop < rhs.prop) {
    return -1;
  } else if (lhs.prop > rhs.prop) {
    return 1;
  }

  return 0;
}

int ComparePaths(const IPath& lhs, const IPath& rhs) {
  return CompareParsedPaths(
      MakeParsedSimplePath(lhs.GetPrimPart(), lhs.GetPropertyPart()),
      MakeParsedSimplePath(rhs.GetPrimPart(), rhs.GetPropertyPart()));
}

void SortSimplePaths(std::vector<SimplePath>& paths) {
  // Schwartzian transform: parse each path once, sort an index array, apply
  // the permutation (ComparePaths would re-parse per comparison).
  const size_t n = paths.size();
  std::vector<ParsedSimplePath> keys;
  keys.reserve(n);
  for (const auto& p : paths) {
    keys.push_back(MakeParsedSimplePath(p.GetPrimPart(), p.GetPropertyPart()));
  }
  std::vector<uint32_t> order(n);
  for (size_t i = 0; i < n; i++) {
    order[i] = uint32_t(i);
  }
  std::sort(order.begin(), order.end(), [&keys](uint32_t a, uint32_t b) {
    return CompareParsedPaths(keys[a], keys[b]) < 0;
  });
  std::vector<SimplePath> sorted;
  sorted.reserve(n);
  for (size_t i = 0; i < n; i++) {
    sorted.push_back(std::move(paths[order[i]]));
  }
  paths = std::move(sorted);
}

} // namespace crate

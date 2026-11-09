// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "layer-diff.hh"
#include "layer-diff-internal.hh"
#include <algorithm>
#include <cstdio>
#include <set>
#include <sstream>

namespace lightusd {
namespace next {
using diff_detail::JoinPrimPath;

namespace {
std::string EscapeJSON(const std::string &str) {
  std::string result;
  for (char c : str) {
    switch (c) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c & 0xff);
          result += buf;
        } else {
          result += c;
        }
        break;
    }
  }
  return result;
}

}  // namespace

std::pair<std::string, std::string> CenterValuePairForDiff(
    const std::string &lhs, const std::string &rhs, size_t window) {
  if (lhs.size() <= window && rhs.size() <= window) {
    return {lhs, rhs};
  }
  // First differing byte offset.
  size_t d = 0;
  const size_t n = std::min(lhs.size(), rhs.size());
  while (d < n && lhs[d] == rhs[d]) ++d;
  // Keep ~1/3 of the window as leading context before the difference.
  const size_t lead = window / 3;
  const size_t start = (d > lead) ? (d - lead) : 0;
  auto clip = [&](const std::string &s) -> std::string {
    std::string out;
    if (start > 0) out += "\xE2\x80\xA6";  // UTF-8 ellipsis
    const size_t end = std::min(s.size(), start + window);
    if (start < s.size()) out += s.substr(start, end - start);
    if (end < s.size()) out += "\xE2\x80\xA6";
    return out;
  };
  return {clip(lhs), clip(rhs)};
}

std::string DiffToText(const Layer &lhs, const Layer &rhs,
                       const std::string &lhs_name,
                       const std::string &rhs_name, const DiffOptions &opts) {
  std::unordered_map<std::string, PrimSpecDiff> psDiffs;
  std::unordered_map<std::string, PropDiff> propDiffs;
  LayerMetaDiff layerMetaDiff;

  Diff(lhs, rhs, psDiffs, propDiffs, opts, &layerMetaDiff);

  auto joinReasons = [](const std::vector<std::string> &reasons) {
    std::stringstream r;
    for (size_t i = 0; i < reasons.size(); ++i) {
      if (i) r << ", ";
      r << reasons[i];
    }
    return r.str();
  };

  bool any = false;
  std::stringstream ss;
  ss << "--- " << lhs_name << "\n";
  ss << "+++ " << rhs_name << "\n";

  // Stage (layer) metadata differences.
  if (layerMetaDiff.changed()) {
    any = true;
    ss << "~ <stage metadata> (Stage metadata modified: "
       << joinReasons(layerMetaDiff.changedFields) << ")\n";
    for (const auto &d : layerMetaDiff.details) {
      ss << "  - " << d.name << ": " << d.lhs << "\n";
      ss << "  + " << d.name << ": " << d.rhs << "\n";
    }
  }

  // Sort paths for consistent output.
  std::set<std::string> sortedPaths;
  for (const auto &entry : psDiffs) sortedPaths.insert(entry.first);
  for (const auto &entry : propDiffs) sortedPaths.insert(entry.first);

  for (const std::string &path : sortedPaths) {
    auto psIt = psDiffs.find(path);
    if (psIt != psDiffs.end()) {
      const PrimSpecDiff &psDiff = psIt->second;

      for (const std::string &name : psDiff.deletedPS) {
        any = true;
        ss << "- " << JoinPrimPath(path, name) << " (PrimSpec deleted)\n";
      }
      for (const std::string &name : psDiff.addedPS) {
        any = true;
        ss << "+ " << JoinPrimPath(path, name) << " (PrimSpec added)\n";
      }
      // Prefer modifiedDetails (carry reasons); fall back to modifiedPS.
      if (!psDiff.modifiedDetails.empty()) {
        for (const ModifiedPrimSpec &m : psDiff.modifiedDetails) {
          any = true;
          ss << "~ " << JoinPrimPath(path, m.name) << " (PrimSpec modified";
          if (!m.reasons.empty()) ss << ": " << joinReasons(m.reasons);
          ss << ")\n";
        }
      } else {
        for (const std::string &name : psDiff.modifiedPS) {
          any = true;
          ss << "~ " << JoinPrimPath(path, name) << " (PrimSpec modified)\n";
        }
      }
    }

    auto propIt = propDiffs.find(path);
    if (propIt != propDiffs.end()) {
      const PropDiff &propDiff = propIt->second;

      for (const std::string &name : propDiff.deletedProps) {
        any = true;
        ss << "- " << path << "." << name << " (Property deleted)\n";
      }
      for (const std::string &name : propDiff.addedProps) {
        any = true;
        ss << "+ " << path << "." << name << " (Property added)\n";
      }
      for (const PropDiff::ModifiedProp &modified :
           propDiff.modifiedPropDetails) {
        any = true;
        ss << "~ " << path << "." << modified.name << " (Property modified";
        if (!modified.reasons.empty()) {
          ss << ": " << joinReasons(modified.reasons);
        }
        ss << ")\n";
        // Center long values on the first difference so a shared prefix (e.g.
        // asset paths) does not hide what actually changed.
        auto pr = CenterValuePairForDiff(modified.lhs, modified.rhs);
        ss << "  - " << pr.first << "\n";
        ss << "  + " << pr.second << "\n";
      }
    }
  }

  if (!any) {
    return "No differences found.\n";
  }

  return ss.str();
}

std::string DiffToJSON(const Layer &lhs, const Layer &rhs,
                       const std::string &lhs_name,
                       const std::string &rhs_name, const DiffOptions &opts) {
  std::unordered_map<std::string, PrimSpecDiff> psDiffs;
  std::unordered_map<std::string, PropDiff> propDiffs;
  LayerMetaDiff layerMetaDiff;

  Diff(lhs, rhs, psDiffs, propDiffs, opts, &layerMetaDiff);

  auto emitStrArray = [](std::stringstream &o,
                         const std::vector<std::string> &v) {
    o << "[";
    for (size_t i = 0; i < v.size(); ++i) {
      if (i > 0) o << ", ";
      o << "\"" << EscapeJSON(v[i]) << "\"";
    }
    o << "]";
  };

  // Deterministic key order.
  std::vector<std::string> psKeys, propKeys;
  psKeys.reserve(psDiffs.size());
  propKeys.reserve(propDiffs.size());
  for (const auto &e : psDiffs) psKeys.push_back(e.first);
  for (const auto &e : propDiffs) propKeys.push_back(e.first);
  std::sort(psKeys.begin(), psKeys.end());
  std::sort(propKeys.begin(), propKeys.end());

  std::stringstream ss;
  ss << "{\n";
  ss << "  \"comparison\": {\n";
  ss << "    \"left\": \"" << EscapeJSON(lhs_name) << "\",\n";
  ss << "    \"right\": \"" << EscapeJSON(rhs_name) << "\"\n";
  ss << "  },\n";

  // PrimSpec differences.
  ss << "  \"primspec_diffs\": {\n";
  bool firstPrimDiff = true;
  for (const std::string &path : psKeys) {
    const PrimSpecDiff &diff = psDiffs[path];
    if (!firstPrimDiff) ss << ",\n";
    firstPrimDiff = false;

    ss << "    \"" << EscapeJSON(path) << "\": {\n";

    ss << "      \"added\": ";
    emitStrArray(ss, diff.addedPS);
    ss << ",\n";

    ss << "      \"deleted\": ";
    emitStrArray(ss, diff.deletedPS);
    ss << ",\n";

    ss << "      \"modified\": ";
    emitStrArray(ss, diff.modifiedPS);
    ss << ",\n";

    ss << "      \"modified_details\": [";
    for (size_t i = 0; i < diff.modifiedDetails.size(); ++i) {
      if (i > 0) ss << ", ";
      ss << "{\"name\":\"" << EscapeJSON(diff.modifiedDetails[i].name)
         << "\", \"reasons\":";
      emitStrArray(ss, diff.modifiedDetails[i].reasons);
      ss << "}";
    }
    ss << "]\n";

    ss << "    }";
  }
  ss << "\n  },\n";

  // Property differences.
  ss << "  \"property_diffs\": {\n";
  bool firstPropDiff = true;
  for (const std::string &path : propKeys) {
    const PropDiff &diff = propDiffs[path];
    if (!firstPropDiff) ss << ",\n";
    firstPropDiff = false;

    ss << "    \"" << EscapeJSON(path) << "\": {\n";

    ss << "      \"added\": ";
    emitStrArray(ss, diff.addedProps);
    ss << ",\n";

    ss << "      \"deleted\": ";
    emitStrArray(ss, diff.deletedProps);
    ss << ",\n";

    ss << "      \"modified\": ";
    emitStrArray(ss, diff.modifiedProps);
    ss << ",\n";

    ss << "      \"modified_details\": [";
    for (size_t i = 0; i < diff.modifiedPropDetails.size(); ++i) {
      if (i > 0) ss << ", ";
      const PropDiff::ModifiedProp &modified = diff.modifiedPropDetails[i];
      ss << "{\"name\":\"" << EscapeJSON(modified.name)
         << "\", \"left\":\"" << EscapeJSON(modified.lhs)
         << "\", \"right\":\"" << EscapeJSON(modified.rhs)
         << "\", \"reasons\":";
      emitStrArray(ss, modified.reasons);
      ss << "}";
    }
    ss << "]\n";

    ss << "    }";
  }
  ss << "\n  },\n";

  // Stage / layer metadata differences.
  ss << "  \"layer_meta_diff\": {\n";
  ss << "    \"changed\": ";
  emitStrArray(ss, layerMetaDiff.changedFields);
  ss << ",\n";
  ss << "    \"details\": [";
  for (size_t i = 0; i < layerMetaDiff.details.size(); ++i) {
    if (i > 0) ss << ", ";
    const PropDiff::ModifiedProp &d = layerMetaDiff.details[i];
    ss << "{\"name\":\"" << EscapeJSON(d.name)
       << "\", \"left\":\"" << EscapeJSON(d.lhs)
       << "\", \"right\":\"" << EscapeJSON(d.rhs) << "\"}";
  }
  ss << "]\n";
  ss << "  }\n";

  ss << "}\n";

  return ss.str();
}

}  // namespace next
}  // namespace lightusd

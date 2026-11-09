// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#pragma once

#include <cstdlib>
#include <string>

#include "dtoa.hh"
#include "value-printer.hh"
#include "../layer/arc-reference.hh"

namespace lightusd {
namespace next {

// Render the internal arc spelling as canonical USDA text. Arc strings are
// stored as "@asset@</prim>", "</prim>", or a bare asset path, optionally
// followed by "?layerOffset=offset:scale".
inline std::string FormatArcRef(const std::string& arc) {
  std::string body = ArcReferenceBody(arc);
  const std::string custom_data = ArcReferenceCustomData(arc);
  std::string suffix;
  const size_t q = body.find("?layerOffset=");
  if (q != std::string::npos) {
    const char* c = body.c_str() + q + 13;
    char* endp = nullptr;
    const double off = std::strtod(c, &endp);
    const double scl = (endp && *endp == ':')
                           ? std::strtod(endp + 1, nullptr)
                           : 1.0;
    body.resize(q);
    if (off != 0.0 || scl != 1.0) {
      suffix = " (";
      if (off != 0.0) {
        suffix += "offset = " + dtos(off);
        if (scl != 1.0) suffix += "; ";
      }
      if (scl != 1.0) suffix += "scale = " + dtos(scl);
      suffix += ")";
    }
  }

  std::string out;
  if (!body.empty() && body[0] == '<') {
    out = body;
  } else if (!body.empty() && body[0] == '@') {
    size_t close = body.find('@', 1);
    std::string asset =
        (close != std::string::npos) ? body.substr(1, close - 1) : "";
    std::string rest =
        (close != std::string::npos) ? body.substr(close + 1) : "";
    if (asset.find('@') != std::string::npos || rest.find('@') == 0) {
      const size_t lt = body.find('<');
      const size_t last_at =
          (lt == std::string::npos ? body : body.substr(0, lt)).rfind('@');
      if (last_at != std::string::npos && last_at > 0) {
        asset = body.substr(1, last_at - 1);
        rest = body.substr(last_at + 1);
        out = FormatAssetPathForUsda(asset) + rest;
      } else {
        out = FormatAssetPathForUsda(body);
      }
    } else {
      out = body;
    }
  } else {
    out = FormatAssetPathForUsda(body);
  }
  if (!custom_data.empty()) {
    if (suffix.empty()) suffix = " (customData = " + custom_data + ")";
    else {
      suffix.pop_back();
      suffix += "; customData = " + custom_data + ")";
    }
  }
  return out + suffix;
}

}  // namespace next
}  // namespace lightusd

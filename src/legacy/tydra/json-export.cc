// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
#include <sstream>
#include <numeric>
#include <unordered_set>

#include "json-export.hh"
#include "common-macros.inc"
#include "tiny-format.hh"

namespace lightusd {
namespace tydra {


bool export_to_json(const RenderScene &scene, bool asset_as_binary,
  std::string &json_str, std::string &binary_str, std::string *warn, std::string *err) {

  (void)scene;
  (void)asset_as_binary;
  (void)json_str;
  (void)binary_str;
  (void)warn;
  (void)err;

  // TODO

  return false;
}


} // namespace tydra
} // namespace lightusd


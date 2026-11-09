// SPDX-License-Identifier: MIT
//
// Work-in-progress. Nothing here yet.
// Built-in .fbx import plugIn using OpenFBX.
// Import only.
//
// example usage
//
// def Xform "fbxroot" (
//   prepend references = @bunny.fbx@
// )
// {
//    ...
// }

#pragma once

#include <string>

class GPrim;

namespace lightusd {

namespace usdFbx {

bool ReadFbxFromString(const std::string &str, GPrim *prim, std::string *err = nullptr);
bool ReadFbxFromFile(const std::string &filepath, GPrim *prim, std::string *err = nullptr);

} // namespace usdFbx

} // namespace lightusd

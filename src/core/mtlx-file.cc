// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.
//
// File-system front ends of the C11 MaterialX documents (lightusd-mtlx.h):
// XInclude loading through LightUSD's asset resolver and security policy, and
// file reading. These were MtlxDocument::ParseFromXML(xml, anchor) and
// MtlxDocument::ParseFromFile of the former src/core/mtlx-dom.cc; everything
// else lives in src/c/mtlx/.

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "c/mtlx/mtlx-doc.h"
#include "core/native-io.hh"
#include "core/security-policy.hh"
#include "resolver/asset-resolver.hh"

namespace {

// MtlxIncludeAnchor: the document an include is resolved relative to.
std::string IncludeAnchor(const std::string &root_path, const std::string &including_href) {
  if (including_href.empty()) return root_path;
  const size_t slash = root_path.find_last_of("/\\");
  if (slash == std::string::npos) return including_href;
  return root_path.substr(0, slash + 1) + including_href;
}

struct LoaderContext {
  lightusd::AssetResolver resolver;
  std::string anchor_path;
};

int ResolverLoader(void *user, const char *href, const char *including_href, char **xml,
                   size_t *len) {
  auto *ctx = static_cast<LoaderContext *>(user);
  std::string normalized;
  if (!lightusd::security_policy::ValidateAndNormalizeAssetPath(href, &normalized)) return 0;
  const std::string source_path = IncludeAnchor(ctx->anchor_path, including_href);
  const lightusd::ResolvedAsset asset = ctx->resolver.Resolve(normalized, source_path, false);
  if (!asset.exists) return 0;
  std::vector<uint8_t> bytes;
  if (!ctx->resolver.ReadAsset(asset, &bytes) || bytes.size() > 256u * 1024u * 1024u) return 0;
  char *buf = static_cast<char *>(std::malloc(bytes.size() + 1));
  if (!buf) return 0;
  if (!bytes.empty()) std::memcpy(buf, bytes.data(), bytes.size());
  buf[bytes.size()] = '\0';
  *xml = buf;
  *len = bytes.size();
  return 1;
}

}  // namespace

extern "C" int lu_mtlx_document_parse_anchored(lu_mtlx_document *doc, const char *xml,
                                               size_t len, const char *anchor_path) {
  LoaderContext ctx;
  ctx.anchor_path = anchor_path ? anchor_path : "";
  return lu_mtlx_document_parse_with_loader(doc, xml, len, ResolverLoader, &ctx);
}

extern "C" int lu_mtlx_document_parse_file(lu_mtlx_document *doc, const char *path) {
  std::string error;
  if (!lightusd::NativeIOAvailable(&error)) {
    lu_mtlx_doc_set_error(doc, "", error.c_str());
    return 0;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    lu_mtlx_doc_set_error(doc, "Failed to open file: ", path);
    return 0;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  const std::string text = buffer.str();
  return lu_mtlx_document_parse_anchored(doc, text.data(), text.size(), path);
}

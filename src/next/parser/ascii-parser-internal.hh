// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - private USDA ASCII parser implementation details.

#pragma once

#include "ascii-parser.hh"
#include "lexer.hh"
#include "../crate/lazy-array.hh"
#include "../layer/layer.hh"

#include <memory>
#include <string>
#include <vector>

namespace lightusd {
namespace next {

class AsciiParser::Impl {
public:
  explicit Impl(const ParseOptions& options) : options_(options) {}

  bool Parse(const char* data, size_t length);
  bool ParseOwned(std::string&& data);
  bool ParseWithSource(const char* data, size_t length,
                      std::shared_ptr<LazyArraySource> source);
  bool ParseFile(const char* filename);

  Stage TakeStage() { return std::move(stage_); }
  const std::vector<ParseError>& GetErrors() const { return errors_; }
  bool HasErrors() const { return !errors_.empty(); }
  const std::vector<std::string>& GetWarnings() const { return warnings_; }

private:
  ParseOptions options_;
  Stage stage_;
  std::vector<ParseError> errors_;
  std::vector<std::string> warnings_;
  std::shared_ptr<LazyArraySource> source_;
  size_t parse_length_ = 0;

  // Parsing state
  std::unique_ptr<Lexer> lexer_;
  std::unique_ptr<Layer> layer_;
  std::unique_ptr<LayerBuilder> builder_;
  size_t depth_ = 0;

  // Parsing methods
  bool ParseStageMetadata();
  bool ParsePrim();
  bool ParsePrimContents();
  bool ParseAttribute(
      PrimSpec::RelationshipListOp connection_op =
          PrimSpec::RelationshipListOp::Append,
      bool explicit_connection = true,
      const std::string& preconsumed_type = std::string());
  bool ParseRelationship(PrimSpec::RelationshipListOp op =
                             PrimSpec::RelationshipListOp::Append,
                         bool explicit_list = true, uint16_t flags = 0);
  bool ParseMetadataBlock();
  bool ParseTimeSamples(const std::string& prop_name, TypeId type_id,
                        bool is_array);
  bool ParseVariantSetBody(const std::string& variant_set_name);
  bool ParseVariantSetBodyInto(const std::string& variant_set_name,
                               std::vector<VariantSetData>& target, int depth);
  bool ParseVariantOption(VariantData* out, int depth);
  // Read one composition-arc reference into canonical "@asset@</prim>" /
  // "</prim>" form (with an optional `?layerOffset=off:scale` suffix). Shared by
  // prim-metadata arcs and variant-option arcs. Returns false if no arc token.
  bool ReadArcRef(std::string* out);
  bool ParseNamespacedName(std::string* out, const char* what);
  bool ParseOrderList(std::vector<std::string>* out);
  bool SkipBalancedBlock(TokenType open, TokenType close, size_t depth = 0);
  bool SkipValueLike();
  void SkipPropertyMetadata();
  void ParsePropertyMetadata(const std::string& prop_name);

  void AddError(const std::string& message);
  void AddWarning(const std::string& message);
  bool ReportProgress(const char* phase, size_t current, size_t total);

  // Token helpers
  bool Match(TokenType type);
  bool Check(TokenType type);
  bool AtEnd();
};

bool IsNameToken(const Token& tok);

}  // namespace next
}  // namespace lightusd

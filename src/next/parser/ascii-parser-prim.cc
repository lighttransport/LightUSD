// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USDA ASCII parser prim/property bodies.

#include "ascii-parser-internal.hh"
#include "../prim/identifier.hh"
#include "../types/spline.hh"
#include "value-parser.hh"
#include "value-parser-numeric.hh"

namespace lightusd {
namespace next {

// Grammar and type validation plus canonical-text reconstruction for a
// VtArrayEdit value: `edit [ <op>; ... ]` (usda 1.2, crate 0.14). The next
// core does not model VtArrayEdit; the edit is preserved as pxr's canonical
// one-line `; `-separated spelling in the attribute's raw default source (the
// usda writer re-emits it verbatim) -- the same keep-don't-evaluate policy as
// the next crate reader, which warns and drops array-edit reps. Literals are
// parsed with the array's ELEMENT type, so a type error inside an edit body
// fails the parse exactly like a plain array literal would.
bool ParseArrayEditText(Lexer& lexer, TypeId elem_type,
                        std::string* canonical, ArrayEditData* out_edit,
                        std::string* err) {
  auto fail = [&](const std::string& m) {
    if (err) *err = "array edit: " + m;
    return false;
  };
  lexer.next();  // `edit` keyword (verified by the caller)
  if (!lexer.expect(TokenType::OpenBracket)) {
    return fail("expected `[` after `edit`");
  }

  // `[ <int> ]` index reference (End-relative negatives are grammatical).
  auto parse_index = [&](std::string* txt, int64_t* val) -> bool {
    if (!lexer.expect(TokenType::OpenBracket)) return false;
    const Token& n = lexer.peek();
    if (n.type != TokenType::Number ||
        !value_parser_detail::IsDecimalIntToken(n.value, /*allow_neg=*/true) ||
        !value_parser_detail::DecimalToI64Checked(n.value.c_str(), val)) {
      return false;
    }
    *txt = "[" + n.value + "]";
    lexer.next();
    return lexer.expect(TokenType::CloseBracket);
  };

  // A literal element value, captured as its authored text span.
  auto parse_literal = [&](std::string* txt) -> bool {
    lexer.peek();
    const size_t start = lexer.token_start();
    const ParseResult r = ParseValue(lexer, elem_type);
    if (!r.success) return false;
    lexer.peek();
    size_t vend = lexer.token_start();
    const char* base = lexer.input_data();
    while (vend > start &&
           (base[vend - 1] == ' ' || base[vend - 1] == '\t' ||
            base[vend - 1] == '\r' || base[vend - 1] == '\n')) {
      --vend;
    }
    txt->assign(base + start, vend - start);
    return true;
  };

  auto expect_word = [&](const char* w) -> bool {
    const Token& t = lexer.peek();
    if (t.type != TokenType::Identifier || t.value != w) return false;
    lexer.next();
    return true;
  };

  std::string out = "edit [";
  bool first = true;
  while (true) {
    const Token& tok = lexer.peek();
    if (tok.type == TokenType::Eof) {
      return fail("unexpected EOF in edit body");
    }
    if (tok.type == TokenType::CloseBracket) {
      lexer.next();
      break;
    }
    // Ops are separated by `;` and/or newlines (the lexer discards the
    // latter); pxr's grammar also admits `,`.
    if (tok.type == TokenType::Semicolon || tok.type == TokenType::Comma) {
      lexer.next();
      continue;
    }
    // `append` / `prepend` lex as list-edit keyword tokens, not identifiers.
    std::string kw;
    if (tok.type == TokenType::Identifier) {
      kw = tok.value;
    } else if (tok.type == TokenType::Append) {
      kw = "append";
    } else if (tok.type == TokenType::Prepend) {
      kw = "prepend";
    } else {
      return fail("expected an edit op keyword");
    }
    lexer.next();

    // Array elements never start with `[` (the element type is scalar or
    // tuple), so a `[` after the op keyword is always an index reference.
    auto next_is_index = [&]() {
      return lexer.peek().type == TokenType::OpenBracket;
    };

    std::string op;
    ArrayEditOpRec rec;
    if (kw == "write" || kw == "insert") {
      const char* conj = (kw == "write") ? "to" : "at";
      const bool is_write = (kw == "write");
      std::string a1;
      if (next_is_index()) {
        if (!parse_index(&a1, &rec.a1)) {
          return fail("bad source index in `" + kw + "`");
        }
        rec.kind = is_write ? ArrayEditOpRec::WriteRef
                            : ArrayEditOpRec::InsertRef;
      } else if (!parse_literal(&a1)) {
        return fail("bad literal in `" + kw + "`");
      } else {
        rec.kind = is_write ? ArrayEditOpRec::WriteLiteral
                            : ArrayEditOpRec::InsertLiteral;
        rec.literal = a1;
      }
      if (!expect_word(conj)) {
        return fail("expected `" + std::string(conj) + "` in `" + kw + "`");
      }
      std::string a2;
      if (!parse_index(&a2, &rec.a2)) {
        return fail("bad target index in `" + kw + "`");
      }
      op = kw + " " + a1 + " " + conj + " " + a2;
    } else if (kw == "append" || kw == "prepend") {
      std::string a1;
      if (next_is_index()) {
        if (!parse_index(&a1, &rec.a1)) {
          return fail("bad index in `" + kw + "`");
        }
        rec.kind = ArrayEditOpRec::InsertRef;
      } else if (!parse_literal(&a1)) {
        return fail("bad literal in `" + kw + "`");
      } else {
        rec.kind = ArrayEditOpRec::InsertLiteral;
        rec.literal = a1;
      }
      rec.a2 = (kw == "append") ? kArrayEditEnd : 0;
      op = kw + " " + a1;
    } else if (kw == "erase") {
      std::string a1;
      if (!parse_index(&a1, &rec.a1)) return fail("bad index in `erase`");
      rec.kind = ArrayEditOpRec::Erase;
      op = "erase " + a1;
    } else if (kw == "minsize" || kw == "resize" || kw == "maxsize") {
      const Token& n = lexer.peek();
      if (n.type != TokenType::Number ||
          !value_parser_detail::IsDecimalIntToken(n.value,
                                                  /*allow_neg=*/false) ||
          !value_parser_detail::DecimalToI64Checked(n.value.c_str(),
                                                    &rec.a1)) {
        return fail("expected a size in `" + kw + "`");
      }
      rec.kind = (kw == "minsize")  ? ArrayEditOpRec::MinSize
                 : (kw == "resize") ? ArrayEditOpRec::SetSize
                                    : ArrayEditOpRec::MaxSize;
      op = kw + " " + n.value;
      lexer.next();
      if (kw != "maxsize" && lexer.peek().type == TokenType::Identifier &&
          lexer.peek().value == "fill") {
        lexer.next();
        std::string f;
        if (!parse_literal(&f)) return fail("bad fill literal in `" + kw + "`");
        rec.has_fill = true;
        rec.literal = f;
        op += " fill " + f;
      }
    } else {
      return fail("unknown edit op `" + kw + "`");
    }
    if (out_edit) out_edit->ops.push_back(std::move(rec));
    if (!first) out += "; ";
    out += op;
    first = false;
  }
  out += "]";
  *canonical = std::move(out);
  return true;
}

namespace {

// Resolve a relative prim/property path (../Sibling, ./Child, Child.attr,
// .prop, ..) against the owning prim's absolute path. Crate files never
// hold relative paths, and downstream lookups are absolute-only — resolve
// at parse time (pxr does the same when building SdfPaths).
std::string ResolveRelativeTargetPath(const ::lightusd::next::PrimSpec* prim,
                                      const std::string& target) {
  if (target.empty() || target[0] == '/' || !prim) return target;
  std::string base = prim->path().str();
  // Variant-option content parses into an internal layer rooted at
  // /__self__; the final host path is unknown until composition, so keep
  // the authored relative form (GraftSubtree remaps it on selection) —
  // resolving here would leak the sentinel into written layers.
  if (base.rfind("/__self__", 0) == 0) return target;
  // ".prop" / "." anchor forms: a leading dot NOT followed by '.' or '/'
  // names a property of the anchor prim itself.
  if (target[0] == '.') {
    if (target.size() == 1) return base;                      // "."
    if (target[1] != '.' && target[1] != '/') return base + target;  // ".prop"
  }
  std::string rest = target;
  while (!rest.empty()) {
    const bool bare_up = (rest == "..");
    if (bare_up || rest.rfind("../", 0) == 0) {
      if (base.empty() || base == "/") return target;  // past root: keep authored
      const size_t up = base.rfind('/');
      base = (up == std::string::npos || up == 0) ? "/" : base.substr(0, up);
      rest = bare_up ? std::string() : rest.substr(3);
    } else if (rest == ".") {
      rest.clear();
    } else if (rest.rfind("./", 0) == 0) {
      rest = rest.substr(2);
    } else {
      break;
    }
  }
  if (rest.empty()) return base;
  if (base.empty() || base == "/") return "/" + rest;
  return base + "/" + rest;
}

}  // namespace

bool AsciiParser::Impl::ParsePrim() {
  if (depth_ >= options_.max_depth) {
    AddError("Maximum prim nesting depth exceeded");
    return false;
  }

  PrimSpecifier specifier = PrimSpecifier::Def;
  const Token& spec_tok = lexer_->peek();
  if (spec_tok.type == TokenType::Def) {
    specifier = PrimSpecifier::Def;
    lexer_->next();
  } else if (spec_tok.type == TokenType::Over) {
    specifier = PrimSpecifier::Over;
    lexer_->next();
  } else if (spec_tok.type == TokenType::Class) {
    specifier = PrimSpecifier::Class;
    lexer_->next();
  } else {
    AddError("Expected 'def', 'over', or 'class'");
    return false;
  }

  std::string type_name;
  if (Check(TokenType::Identifier)) {
    lexer_->expect(TokenType::Identifier, type_name);
  }

  std::string prim_name;
  if (!lexer_->expect(TokenType::String, prim_name)) {
    AddError("Expected prim name");
    return false;
  }

  if (!IsValidIdentifier(prim_name)) {
    // AOUSD §7.3/§8 uses Unicode XID_Start/XID_Continue. Reuse the same
    // generated XID tables as legacy LightUSD rather than bytewise ctype.
    AddError("Prim name contains invalid character(s): \"" + prim_name + "\"");
    return false;
  }

  // `__AnyType__` is legacy spelling for "no prim type" — pxr composes and
  // re-serializes it as an untyped prim.
  if (type_name == "__AnyType__") type_name.clear();
  builder_->begin_prim(prim_name, type_name, specifier);

  if (Check(TokenType::OpenParen)) {
    if (!ParseMetadataBlock()) {
      builder_->end_prim();
      return false;
    }
  }

  if (!Match(TokenType::OpenBrace)) {
    AddError("Expected '{' to start prim body");
    builder_->end_prim();
    return false;
  }

  depth_++;
  if (!ParsePrimContents()) {
    depth_--;
    builder_->end_prim();
    return false;
  }
  depth_--;

  if (!Match(TokenType::CloseBrace)) {
    AddError("Expected '}' to close prim body");
    builder_->end_prim();
    return false;
  }

  builder_->end_prim();
  return ReportProgress("prims", lexer_->position(), parse_length_);
}

bool AsciiParser::Impl::ParsePrimContents() {
  if (!builder_->current()) return false;

  while (!Check(TokenType::CloseBrace) && !AtEnd()) {
    // pxr accepts `;` as an optional statement separator in prim bodies.
    if (Match(TokenType::Semicolon)) continue;
    const Token& tok = lexer_->peek();

    // Re-fetch each iteration: parsing a nested child prim (below) appends to
    // the layer's flat prim vector, which can reallocate and invalidate any
    // PrimSpec* cached across the call. current_index_ is stable, so
    // current() always resolves to this prim's live storage.
    PrimSpec* prim = builder_->current();
    if (!prim) return false;

    if (tok.type == TokenType::Def || tok.type == TokenType::Over ||
        tok.type == TokenType::Class) {
      if (!ParsePrimMaybeParallel()) {
        return false;
      }
      continue;
    }

    if (tok.type == TokenType::Rel) {
      if (!ParseRelationship()) {
        return false;
      }
      continue;
    }

    if (tok.type == TokenType::Reorder) {
      lexer_->next();
      if (lexer_->peek().type == TokenType::Rel) {
        if (!ParseRelationship(PrimSpec::RelationshipListOp::Reorder,
                               /*explicit_list=*/false)) return false;
        continue;
      }
      std::string what;
      if (!lexer_->expect(TokenType::Identifier, what)) return false;
      if (what == "nameChildren" || what == "primChildren") {
        if (!ParseOrderList(&prim->meta().editPrimOrder())) return false;
        prim->meta().setPrimOrderAuthored();
      } else if (what == "properties" || what == "propertyChildren") {
        if (!ParseOrderList(&prim->meta().editPropertyOrder())) return false;
        prim->meta().setPropertyOrderAuthored();
      } else {
        if (!ParseAttribute(PrimSpec::RelationshipListOp::Reorder,
                            /*explicit_connection=*/false, what)) return false;
      }
      continue;
    }

    if (tok.type == TokenType::Prepend || tok.type == TokenType::Append ||
        tok.type == TokenType::Delete || tok.type == TokenType::Add) {
      TokenType op_tok = tok.type;
      lexer_->next();
      if (lexer_->peek().type == TokenType::Rel) {
        PrimSpec::RelationshipListOp op = PrimSpec::RelationshipListOp::Append;
        if (op_tok == TokenType::Prepend) {
          op = PrimSpec::RelationshipListOp::Prepend;
        } else if (op_tok == TokenType::Delete) {
          op = PrimSpec::RelationshipListOp::Delete;
        } else if (op_tok == TokenType::Add) {
          op = PrimSpec::RelationshipListOp::Add;
        }
        if (!ParseRelationship(op, /*explicit_list=*/false)) return false;
        continue;
      }
      PrimSpec::RelationshipListOp op = PrimSpec::RelationshipListOp::Append;
      if (op_tok == TokenType::Prepend)
        op = PrimSpec::RelationshipListOp::Prepend;
      else if (op_tok == TokenType::Delete)
        op = PrimSpec::RelationshipListOp::Delete;
      else if (op_tok == TokenType::Add)
        op = PrimSpec::RelationshipListOp::Add;
      if (!ParseAttribute(op, /*explicit_connection=*/false)) return false;
      continue;
    }

    if (tok.type == TokenType::Identifier && tok.value == "variantSet") {
      lexer_->next();
      std::string vs_name;
      if (!lexer_->expect(TokenType::String, vs_name)) {
        AddError("Expected variantSet name");
        return false;
      }
      if (!Match(TokenType::Equals)) {
        AddError("Expected '=' in variantSet");
        return false;
      }
      if (!ParseVariantSetBody(vs_name)) {
        return false;
      }
      continue;
    }

    if (tok.type == TokenType::Custom || tok.type == TokenType::Uniform ||
        tok.type == TokenType::Varying || tok.type == TokenType::Identifier) {
      if (!ParseAttribute()) {
        return false;
      }
      continue;
    }

    AddWarning("Skipping unknown token: " + tok.value);
    lexer_->next();
  }

  return true;
}

bool AsciiParser::Impl::ParseAttribute(
    PrimSpec::RelationshipListOp connection_op, bool explicit_connection,
    const std::string& preconsumed_type) {
  bool is_custom = false;
  bool is_uniform = false;
  bool is_varying = false;
  bool variability_authored = false;

  while (true) {
    const Token& tok = lexer_->peek();
    if (tok.type == TokenType::Custom) {
      is_custom = true;
      lexer_->next();
    } else if (tok.type == TokenType::Uniform) {
      is_uniform = true;
      variability_authored = true;
      lexer_->next();
    } else if (tok.type == TokenType::Varying) {
      is_varying = true;
      variability_authored = true;
      lexer_->next();
    } else {
      break;
    }
  }

  if (lexer_->peek().type == TokenType::Rel) {
    uint16_t rflags = 0;
    if (is_custom) rflags |= PropSlot::kFlagCustom;
    if (is_uniform) rflags |= PropSlot::kFlagUniform;
    if (variability_authored) rflags |= PropSlot::kFlagVariabilityAuthored;
    if (is_varying) rflags |= PropSlot::kFlagVarying;
    return ParseRelationship(PrimSpec::RelationshipListOp::Append,
                             /*explicit_list=*/true, rflags);
  }

  std::string type_name = preconsumed_type;
  if (type_name.empty() && !lexer_->expect(TokenType::Identifier, type_name)) {
    AddError("Expected attribute type");
    return false;
  }

  bool is_array = false;
  if (Check(TokenType::OpenBracket)) {
    lexer_->next();
    if (!Match(TokenType::CloseBracket)) {
      AddError("Expected ']' for array type");
      return false;
    }
    is_array = true;
  }

  std::string attr_name;
  if (!ParseNamespacedName(&attr_name, "attribute name")) {
    return false;
  }

  if (PrimSpec* p = builder_->current()) {
    p->set_property_type_name(attr_name,
                              is_array ? type_name + "[]" : type_name);
  }

  ParsePropertyMetadata(attr_name);

  bool dummy_array;
  TypeId type_id = ParseTypeName(type_name, dummy_array);
  if (type_id == TypeId::Invalid && !options_.allow_unknown_types) {
    AddError("Unknown attribute type: " + type_name);
    return false;
  }

  uint16_t flags = 0;
  if (is_custom) flags |= PropSlot::kFlagCustom;
  if (is_uniform) flags |= PropSlot::kFlagUniform;
  if (variability_authored) flags |= PropSlot::kFlagVariabilityAuthored;
  if (is_varying) flags |= PropSlot::kFlagVarying;

  if (Check(TokenType::Equals)) {
    if (!explicit_connection) {
      AddError("List-edit qualifier is valid only for attribute connections");
      return false;
    }
    lexer_->next();

    if (type_id == TypeId::Invalid) {
      // `opaque` and its `group` role cannot carry ordinary serialized values,
      // but AOUSD permits the universal ValueBlock sentinel. Accept that form
      // in strict mode without pretending either type has a concrete codec.
      if ((type_name == "opaque" || type_name == "group") &&
          Check(TokenType::None)) {
        ParseResult result = ParseValue(*lexer_, type_id);
        if (!result.success) {
          AddError(result.error);
          return false;
        }
        if (PrimSpec* cur = builder_->current()) {
          cur->upsert_property(attr_name, std::move(result.value), flags);
        }
        ParsePropertyMetadata(attr_name);
        return true;
      }
      const bool normative_unsupported =
          type_name == "frame4d" || type_name == "opaque" ||
          type_name == "group";
      if (options_.strict_aousd_conformance && normative_unsupported) {
        AddError("Unsupported AOUSD foundational value type in strict mode: " +
                 type_name);
        return false;
      }
      // Preserve the complete authored literal rather than guessing a storage
      // type or silently leaving a declared-only slot. Extension types can
      // therefore round-trip in compatibility mode without corrupting data.
      lexer_->peek();
      const size_t vstart = lexer_->token_start();
      if (!SkipValueLike()) {
        AddError("Failed to preserve value for unsupported attribute type: " +
                 type_name);
        return false;
      }
      lexer_->peek();
      size_t vend = lexer_->token_start();
      const char* base = lexer_->input_data();
      while (vend > vstart &&
             (base[vend - 1] == ' ' || base[vend - 1] == '\t' ||
              base[vend - 1] == '\r' || base[vend - 1] == '\n')) {
        --vend;
      }
      if (PrimSpec* cur = builder_->current()) {
        const PropNameId nid = GetPropNameTable().intern(attr_name);
        std::string raw(base + vstart, vend - vstart);
        // Preserve the former generic-read convenience for unambiguous scalar
        // extension values while retaining the exact authored bytes for write.
        Lexer scalar(raw.data(), raw.size());
        ParseResult inferred;
        const TokenType first = scalar.peek().type;
        if (first == TokenType::Number)
          inferred = ParseValue(scalar, TypeId::Double);
        else if (first == TokenType::String)
          inferred = ParseValue(scalar, TypeId::String);
        else if (first == TokenType::True || first == TokenType::False)
          inferred = ParseValue(scalar, TypeId::Bool);
        if (inferred.success && !inferred.value.is_empty()) {
          cur->upsert_property(nid, std::move(inferred.value), flags);
        } else if (!cur->property(nid)) {
          cur->add_property_slot(nid, type_id, flags);
        }
        cur->set_raw_default_source(attr_name, std::move(raw));
      }
      AddWarning("Unsupported attribute type preserved as raw authored data: " +
                 type_name);
    } else {
      ParseResult result;
      const Token& vtok = lexer_->peek();
      if (is_array && vtok.type == TokenType::Identifier &&
          vtok.value == "edit") {
        // VtArrayEdit value (usda 1.2): keep the canonical text for printing
        // and the structured op list for composition (which stacks weaker
        // edits and resolves against weaker array values -- see
        // layer/array-edit.hh).
        std::string canonical, edit_err;
        ArrayEditData edit_data;
        if (!ParseArrayEditText(*lexer_, type_id, &canonical, &edit_data,
                                &edit_err)) {
          AddError(edit_err);
          return false;
        }
        if (PrimSpec* cur = builder_->current()) {
          const PropNameId nid = GetPropNameTable().intern(attr_name);
          if (!cur->property(nid)) {
            cur->add_property_slot(
                nid, type_id,
                static_cast<uint16_t>(flags | PropSlot::kFlagArray));
          }
          cur->set_raw_default_source(attr_name, std::move(canonical));
          cur->set_array_edit(attr_name, std::move(edit_data));
        }
        ParsePropertyMetadata(attr_name);
        return true;
      }
      if (is_array) {
        result = ParseArrayAttributeValue(type_id, nullptr);
      } else {
        result = ParseValue(*lexer_, type_id);
      }

      if (!result.success) {
        AddError(result.error);
        return false;
      }

      // Upsert: `attr.timeSamples = {...}` authored before `attr = v` already
      // created a slot; a second slot with the same name would emit two
      // Attribute specs with the same path (pxr: "invalid specs: spec
      // repeated" — the whole layer fails to open).
      if (PrimSpec* cur = builder_->current()) {
        cur->upsert_property(attr_name, std::move(result.value), flags);
      }
    }
    ParsePropertyMetadata(attr_name);

  } else if (Check(TokenType::Dot)) {
    lexer_->next();
    const Token& prop_tok = lexer_->peek();
    if (!explicit_connection &&
        !(prop_tok.type == TokenType::Identifier &&
          prop_tok.value == "connect")) {
      AddError("List-edit qualifier is valid only for attribute connections");
      return false;
    }

    if (prop_tok.type == TokenType::TimeSamples) {
      if (!explicit_connection) {
        AddError("List-edit qualifier is valid only for attribute connections");
        return false;
      }
      lexer_->next();
      if (!Match(TokenType::Equals)) {
        AddError("Expected '=' after timeSamples");
        return false;
      }
      flags |= PropSlot::kFlagTimeSampled;
      if (is_array) flags |= PropSlot::kFlagArray;
      if (PrimSpec* cur = builder_->current()) {
        const PropNameId name_id = GetPropNameTable().intern(attr_name);
        if (PropSlot* slot = cur->property_mutable(name_id)) {
          slot->flags |= flags;
        } else {
          cur->add_property_slot(name_id, type_id, flags);
        }
      }
      if (!ParseTimeSamples(attr_name, type_id, is_array)) {
        return false;
      }
      ParsePropertyMetadata(attr_name);
    } else if (prop_tok.type == TokenType::Identifier &&
               prop_tok.value == "connect") {
      lexer_->next();
      if (!Match(TokenType::Equals)) {
        AddError("Expected '=' after connect");
        return false;
      }
      flags |= PropSlot::kFlagConnection;
      if (is_array) flags |= PropSlot::kFlagArray;
      PrimSpec* cur = builder_->current();
      const PropNameId nid = GetPropNameTable().intern(attr_name);
      if (cur && !cur->property(nid)) {
        cur->add_property_slot(nid, type_id, flags);
      }
      std::vector<Path> targets;
      bool authored_empty = false;
      if (Check(TokenType::None)) {
        // `.connect = None`: an authored connection block.
        lexer_->next();
        authored_empty = true;
      } else if (Check(TokenType::OpenBracket)) {
        lexer_->next();
        while (!Check(TokenType::CloseBracket) && !AtEnd()) {
          std::string p;
          if (lexer_->expect(TokenType::PathRef, p) && cur) {
            targets.emplace_back(ResolveRelativeTargetPath(cur, p));
          }
          Match(TokenType::Comma);
        }
        Match(TokenType::CloseBracket);
      } else {
        std::string path;
        if (!lexer_->expect(TokenType::PathRef, path)) {
          AddError("Expected path for connection");
          return false;
        }
        if (cur) targets.emplace_back(ResolveRelativeTargetPath(cur, path));
      }
      if (cur) {
        if (explicit_connection) {
          cur->set_connection_targets(attr_name, std::move(targets));
          ArcEdit& edit = cur->ensure_connection_edit(attr_name);
          edit = ArcEdit();
          edit.authored = true;
          edit.is_explicit = true;
        } else {
          const ArcEdit* prior_edit = cur->connection_edit(attr_name);
          const bool explicit_base =
              prior_edit && prior_edit->authored && prior_edit->is_explicit &&
              cur->connection(attr_name) && !cur->connection(attr_name)->empty();
          cur->apply_connection_list_op(attr_name, targets, connection_op);
          ArcEdit& edit = cur->ensure_connection_edit(attr_name);
          if (explicit_base) {
            edit = ArcEdit();
            edit.authored = true;
            edit.is_explicit = true;
            ParsePropertyMetadata(attr_name);
            return true;
          }
          edit.authored = true;
          edit.is_explicit = false;
          std::vector<std::string>* sublist = nullptr;
          switch (connection_op) {
            case PrimSpec::RelationshipListOp::Add: sublist = &edit.added; break;
            case PrimSpec::RelationshipListOp::Prepend:
              sublist = &edit.prepended;
              break;
            case PrimSpec::RelationshipListOp::Append:
              sublist = &edit.appended;
              break;
            case PrimSpec::RelationshipListOp::Delete:
              sublist = &edit.deleted;
              break;
            case PrimSpec::RelationshipListOp::Reorder:
              sublist = &edit.ordered;
              break;
          }
          if (sublist) {
            for (const Path& target : targets) sublist->push_back(target.str());
          }
        }
        if (authored_empty && explicit_connection) cur->set_connection_block(attr_name);
      }
      ParsePropertyMetadata(attr_name);
    } else if (prop_tok.type == TokenType::Identifier &&
               prop_tok.value == "spline") {
      lexer_->next();
      if (!Match(TokenType::Equals)) {
        AddError("Expected '=' after spline");
        return false;
      }
      // The raw authored text is the storage (lossless round-trip through
      // composition and the USDA writer); the typed view (ParseSplineText)
      // drives evaluation and crate encoding on demand.
      lexer_->peek();
      const size_t vstart = lexer_->token_start();
      if (!SkipValueLike()) {
        AddError("Failed to parse spline field");
        return false;
      }
      lexer_->peek();
      size_t vend = lexer_->token_start();
      const char* base = lexer_->input_data();
      while (vend > vstart &&
             (base[vend - 1] == ' ' || base[vend - 1] == '\t' ||
              base[vend - 1] == '\r' || base[vend - 1] == '\n')) {
        --vend;
      }
      std::string spline_text(base + vstart, vend - vstart);
      // Validate the grammar up front so malformed splines fail at parse time
      // instead of at first evaluation/crate write.
      {
        SplineData sd;
        std::string serr;
        if (!ParseSplineText(spline_text, &sd, &serr)) {
          if (options_.strict_aousd_conformance) {
            AddError("Malformed AOUSD spline value: " + serr);
            return false;
          }
          AddWarning("Spline preserved as raw text but not evaluable: " +
                     serr);
        }
      }
      if (PrimSpec* cur = builder_->current()) {
        const PropNameId nid = GetPropNameTable().intern(attr_name);
        if (!cur->property(nid)) {
          cur->add_property_slot(nid, type_id, flags);
        }
        cur->set_spline_source(attr_name, std::move(spline_text));
      }
    } else {
      // Unknown property suffix: never silently ignore it in strict mode.
      const std::string suffix = prop_tok.value;
      if (options_.strict_aousd_conformance) {
        AddError("Unsupported attribute field in strict AOUSD mode: ." +
                 suffix);
        return false;
      }
      AddWarning("Unknown attribute property: " + suffix);
      lexer_->next();
      if (Match(TokenType::Equals)) SkipValueLike();
    }
  } else {
    if (!explicit_connection) {
      AddError("List-edit qualifier is valid only for attribute connections");
      return false;
    }
    // Bare declaration (`token outputs:out`): declared-only slot with no
    // value; an Invalid-typed Value would emit a type-enum-0 `default` field.
    if (PrimSpec* cur = builder_->current()) {
      const PropNameId nid = GetPropNameTable().intern(attr_name);
      if (!cur->property(nid)) cur->add_property_slot(nid, type_id, flags);
    }
  }

  return true;
}



bool AsciiParser::Impl::ParseRelationship(PrimSpec::RelationshipListOp op,
                                          bool explicit_list,
                                          uint16_t flags) {
  lexer_->next();

  std::string rel_name;
  if (!ParseNamespacedName(&rel_name, "relationship name")) {
    return false;
  }
  if (flags) {
    if (PrimSpec* prim = builder_->current()) {
      prim->set_relationship_flags(
          rel_name, static_cast<uint16_t>(
                        prim->relationship_flags(rel_name) | flags));
    }
  }

  ParsePropertyMetadata(rel_name);

  if (!Match(TokenType::Equals)) {
    // Bare declaration (`custom rel material:binding`): register an empty
    // relationship so it round-trips (writers emit `rel name`, no targets).
    if (PrimSpec* prim = builder_->current()) {
      if (!prim->relationship(rel_name)) {
        prim->set_relationship_targets(rel_name, {});
      }
    }
    return true;
  }

  std::vector<Path> targets;
  const bool value_block = Check(TokenType::None);
  const ::lightusd::next::PrimSpec* owner = builder_->current();
  if (Check(TokenType::OpenBracket)) {
    lexer_->next();
    while (!Check(TokenType::CloseBracket) && !AtEnd()) {
      std::string target;
      if (lexer_->expect(TokenType::PathRef, target)) {
        targets.emplace_back(ResolveRelativeTargetPath(owner, target));
      }
      if (!Check(TokenType::CloseBracket)) {
        Match(TokenType::Comma);
      }
    }
    Match(TokenType::CloseBracket);
  } else if (Check(TokenType::PathRef)) {
    std::string target;
    lexer_->expect(TokenType::PathRef, target);
    targets.emplace_back(ResolveRelativeTargetPath(owner, target));
  } else if (Check(TokenType::None)) {
    lexer_->next();
  }

  if (PrimSpec* prim = builder_->current()) {
    if (targets.empty()) {
      // `= None` / `= []`: keep the relationship declared (empty) rather than
      // dropping it, and retain the authored explicit-empty targetPaths
      // opinion separately from a bare relationship declaration.
      if (!prim->relationship(rel_name)) {
        prim->set_relationship_targets(rel_name, {});
      }
      ArcEdit& e = prim->ensure_relationship_edit(rel_name);
      e = ArcEdit();
      e.authored = true;
      e.is_explicit = explicit_list;
      e.is_value_block = value_block;
    } else {
      // A bare explicit list authored BEFORE any edits dominates: the
      // explicit base is not representable in edit sublists, so subsequent
      // edits keep the legacy flatten-into-effective behavior.
      const bool explicit_base =
          !explicit_list && prim->relationship_edits().count(rel_name) == 0 &&
          prim->relationship(rel_name) && !prim->relationship(rel_name)->empty();
      prim->apply_relationship_list_op(rel_name, targets, op);
      // Record the authored list-op edit so the qualifier round-trips
      // (`prepend rel r = <...>` used to flatten to an explicit list and
      // `delete` edits vanished entirely).
      if (!explicit_list && !explicit_base) {
        ArcEdit& e = prim->ensure_relationship_edit(rel_name);
        e.authored = true;
        e.is_explicit = false;
        std::vector<std::string>* sub = nullptr;
        switch (op) {
          case PrimSpec::RelationshipListOp::Prepend:
            sub = &e.prepended;
            break;
          case PrimSpec::RelationshipListOp::Append:
            sub = &e.appended;
            break;
          case PrimSpec::RelationshipListOp::Add:
            sub = &e.added;
            break;
          case PrimSpec::RelationshipListOp::Delete:
            sub = &e.deleted;
            break;
          case PrimSpec::RelationshipListOp::Reorder:
            sub = &e.ordered;
            break;
        }
        if (sub) {
          for (const Path& t : targets) sub->push_back(t.str());
        }
      } else {
        // A later bare (explicit) authoring replaces any recorded edits.
        if (prim->relationship_edits().count(rel_name)) {
          ArcEdit& e = prim->ensure_relationship_edit(rel_name);
          e = ArcEdit();
          e.authored = true;
        }
      }
    }
  }
  ParsePropertyMetadata(rel_name);

  return true;
}

}  // namespace next
}  // namespace lightusd

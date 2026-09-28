// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USDA ASCII Parser implementation

#include "ascii-parser-internal.hh"
#include "../safe-file-size.hh"
#include "../strfmt.hh"
#include "usda-lazy-source.hh"
#include "value-parser.hh"
#include "../../external/fast_float/include/fast_float/fast_float.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <fstream>
#include <system_error>
#include <limits>

namespace lightusd {
namespace next {
namespace {

ParseOptions NormalizeParseOptions(const ParseOptions& options) {
  ParseOptions normalized = options;
  if (normalized.max_usda_lazy_array_elements == 0) {
    normalized.max_usda_lazy_array_elements =
        std::numeric_limits<size_t>::max();
  }
  if (normalized.num_threads < 0) {
    normalized.num_threads = 0;
  }
  return normalized;
}

}  // namespace

bool IsNameToken(const Token& tok) {
  switch (tok.type) {
    case TokenType::Identifier:
    case TokenType::Def:
    case TokenType::Over:
    case TokenType::Class:
    case TokenType::True:
    case TokenType::False:
    case TokenType::None:
    case TokenType::TimeSamples:
    case TokenType::Custom:
    case TokenType::Uniform:
    case TokenType::Varying:
    case TokenType::Prepend:
    case TokenType::Append:
    case TokenType::Delete:
    case TokenType::Add:
    case TokenType::Reorder:
    case TokenType::Rel:
      return !tok.value.empty();
    default:
      return false;
  }
}

bool AsciiParser::Impl::ReportProgress(const char* phase, size_t current,
                                       size_t total) {
  if (!options_.progress_callback ||
      options_.progress_callback(phase, current, total)) return true;
  AddError(std::string("USDA parse cancelled during ") + phase);
  return false;
}

bool AsciiParser::Impl::ParseWithSource(const char* data, size_t length,
                                       std::shared_ptr<LazyArraySource> source) {
  errors_.clear();
  warnings_.clear();
  depth_ = 0;
  parse_length_ = length;
  source_ = std::move(source);

  if (!source_ && length != 0 && !data) {
    AddError("Invalid null USDA input");
    return false;
  }

  if (options_.max_file_size > 0 && length > options_.max_file_size) {
    AddError("File size exceeds maximum allowed");
    return false;
  }

  // Create fresh layer and builder
  layer_ = std::make_unique<Layer>();
  builder_ = std::make_unique<LayerBuilder>(*layer_);

  if (source_) {
    // Keep a shared ownership of the full USDA source while parsing so any lazy
    // array source slices stay valid until the parse/build graph drops them.
    lexer_ = std::make_unique<Lexer>(
        reinterpret_cast<const char*>(source_->base()), length);
  } else {
    lexer_ = std::make_unique<Lexer>(data, length);
  }
  lexer_->num_threads = options_.num_threads;
  lexer_->strict_aousd_conformance = options_.strict_aousd_conformance;

  if (!ReportProgress("bootstrap", 0, length)) return false;

  // Enforce the `#usda 1.0` magic on the raw bytes BEFORE lexing: the lexer
  // treats '#' as a comment, so a token-level check is dead code and any
  // text starting with `def "x" {}` would "parse" as USDA.
  {
    const char* raw = source_ ? reinterpret_cast<const char*>(source_->base())
                              : data;
    size_t pos = 0;
    while (pos < length &&
           (raw[pos] == ' ' || raw[pos] == '\t' || raw[pos] == '\r' ||
            raw[pos] == '\n')) {
      pos++;
    }
    bool ok = (pos + 5 <= length) && std::memcmp(raw + pos, "#usda", 5) == 0;
    if (ok) {
      pos += 5;
      size_t ws = pos;
      while (ws < length && (raw[ws] == ' ' || raw[ws] == '\t')) ws++;
      // Require whitespace then a version digit ("#usda  1.0" is valid —
      // the legacy parser accepts arbitrary spacing here).
      ok = (ws > pos) && ws < length && raw[ws] >= '0' && raw[ws] <= '9';
    }
    if (!ok) {
      AddError("Missing or invalid '#usda 1.0' header");
      return false;
    }
  }

  // Parse stage metadata (header block)
  if (!ParseStageMetadata()) {
    return false;
  }

  // Parse root prims
  while (!AtEnd()) {
    // Pseudo-root namespace ordering is an authored field and must survive a
    // layer rewrite even when it mentions currently absent prim names.
    if (lexer_->peek().type == TokenType::Reorder) {
      lexer_->next();
      std::string what;
      if (!lexer_->expect(TokenType::Identifier, what)) return false;
      if (what == "rootPrims") {
        if (!ParseOrderList(&layer_->meta().rootPrimOrder)) return false;
        layer_->meta().rootPrimOrder_set = true;
      } else if (options_.strict_aousd_conformance) {
        AddError("Unsupported root reorder field: " + what);
        return false;
      } else {
        if (Match(TokenType::Equals)) SkipValueLike();
        AddWarning("Unknown root reorder field ignored: " + what);
      }
      continue;
    }
    if (!ParsePrim()) {
      return false;
    }
  }

  // A fatal lexical malformation (unterminated string/path/asset literal,
  // oversized token) must fail the parse even when the token-level grammar
  // happened to recover (e.g. an unterminated single-quoted string cut off by
  // a newline used to be silently accepted).
  if (lexer_->has_fatal_error()) {
    AddError(lexer_->error());
    return false;
  }

  // Finalize the layer
  builder_->finalize();

  if (!ReportProgress("complete", length, length)) return false;

  // Create stage from layer
  stage_ = Stage();
  stage_.SetRootLayer(std::move(*layer_));

  lexer_.reset();
  builder_.reset();
  layer_.reset();

  return errors_.empty();
}

bool AsciiParser::Impl::Parse(const char* data, size_t length) {
  if (options_.enable_usda_lazy_arrays) {
    auto source = UsdaLazyArraySource::AdoptString(
        data ? std::string(data, data + length) : std::string());
    const char* src_data = reinterpret_cast<const char*>(source->base());
    return ParseWithSource(src_data, length, std::move(source));
  }
  return ParseWithSource(data, length, nullptr);
}

bool AsciiParser::Impl::ParseOwned(std::string&& data) {
  const size_t length = data.size();
  if (options_.enable_usda_lazy_arrays) {
    auto source = UsdaLazyArraySource::AdoptString(std::move(data));
    const char* src_data = reinterpret_cast<const char*>(source->base());
    return ParseWithSource(src_data, length, std::move(source));
  }
  const char* src_data = data.empty() ? nullptr : data.data();
  return ParseWithSource(src_data, length, nullptr);
}

bool AsciiParser::Impl::ParseFile(const char* filename) {
  if (!filename || !*filename) {
    AddError("Invalid USDA filename");
    return false;
  }
  std::ifstream file(filename, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    AddError(std::string("Failed to open file: ") + filename);
    return false;
  }

  // Unconditional bound -- max_file_size is optional, and the size_t
  // comparison this replaces was a no-op on LP64. A directory opens fine here
  // and reports LLONG_MAX (see SafeStreamSize).
  size_t size = 0;
  if (!SafeStreamSize(file, static_cast<uint64_t>(options_.max_file_size),
                      &size)) {
    AddError("Invalid or oversized file");
    return false;
  }
  file.seekg(0, std::ios::beg);

  if (options_.enable_usda_lazy_arrays) {
    std::string mmap_error;
    auto mapped = UsdaLazyArraySource::MmapFile(filename, &mmap_error);
    if (mapped) {
      const char* data = reinterpret_cast<const char*>(mapped->base());
      const size_t mapped_size = mapped->size();
      return ParseWithSource(data, mapped_size, std::move(mapped));
    }

    std::string content(size ? size : 0, '\0');
    if (size && !file.read(content.data(), static_cast<std::streamsize>(size))) {
      AddError("Failed to read file contents");
      return false;
    }
    auto src = UsdaLazyArraySource::AdoptString(std::move(content));
    const char* data = reinterpret_cast<const char*>(src->base());
    return ParseWithSource(data, size, std::move(src));
  }

  // Default-init (NOT value-init) the buffer: `new char[]` leaves the bytes
  // uninitialized, so we skip zero-filling hundreds of MB we immediately
  // overwrite with file.read (the zero-fill was ~8% of a big-file parse).
  std::unique_ptr<char[]> content(new char[size ? size : 1]);
  if (size && !file.read(content.get(), static_cast<std::streamsize>(size))) {
    AddError("Failed to read file contents");
    return false;
  }

  return Parse(content.get(), size);
}

// Read one composition-arc reference into canonical "@asset@</prim>" /
// "</prim>" form (the lexer yields @asset@ as a String without '@' and
// </prim> as a PathRef without '<>'). Peeks before consuming so a missing
// optional token does not eat the next one. Returns false if no arc token.
bool AsciiParser::Impl::ReadArcRef(std::string* out) {
  std::string ref;
  if (Check(TokenType::String)) {
    std::string asset;
    lexer_->expect(TokenType::String, asset);
    ref = "@" + asset + "@";
    if (Check(TokenType::PathRef)) {
      std::string pr;
      lexer_->expect(TokenType::PathRef, pr);
      ref += "<" + pr + ">";
    }
  } else if (Check(TokenType::PathRef)) {
    std::string pr;
    lexer_->expect(TokenType::PathRef, pr);
    ref = "<" + pr + ">";
  } else {
    return false;
  }
  // Optional per-arc layer offset: `(offset = N; scale = M)`. Encoded into the
  // canonical ref as `?layerOffset=offset:scale` (Compositor::ParseReference
  // decodes it); composition composes it through the arc chain and bakes it
  // into time-sample times.
  if (Check(TokenType::OpenParen)) {
    Match(TokenType::OpenParen);
    double off = 0.0, scl = 1.0;
    while (!Check(TokenType::CloseParen) && !AtEnd()) {
      if (Check(TokenType::Identifier)) {
        std::string k;
        lexer_->expect(TokenType::Identifier, k);
        Match(TokenType::Equals);
        if (Check(TokenType::Number)) {
          std::string num;
          lexer_->expect(TokenType::Number, num);
          // Freestanding double parse (fast_float; no libc strtod).
          double v = 0.0;
          {
            const char* b = num.c_str();
            const char* e = b + num.size();
            auto r = fast_float::from_chars(b, e, v);
            if (!(r.ec == std::errc{} && r.ptr == e) && b < e && *b == '+') {
              fast_float::from_chars(b + 1, e, v);
            }
          }
          if (k == "offset") off = v;
          else if (k == "scale") scl = v;
        } else {
          // Unknown key with a structured value (customData = { ... } may
          // contain nested parens/braces): balanced skip, or the paren scan
          // terminates INSIDE the value and desyncs the metadata block.
          SkipValueLike();
        }
      } else {
        lexer_->next();  // skip unexpected token (avoid spinning)
      }
      Match(TokenType::Semicolon);
    }
    Match(TokenType::CloseParen);
    if (!std::isfinite(off) || !std::isfinite(scl) || !(scl > 0.0)) {
      if (options_.strict_aousd_conformance) {
        AddError("AOUSD composition-arc offset must be finite and scale must "
                 "be finite and greater than zero");
        return false;
      }
      AddWarning("Invalid composition-arc layer offset; using identity mapping");
      off = 0.0;
      scl = 1.0;
    }
    if (off != 0.0 || scl != 1.0) {
      ref += "?layerOffset=" + std::to_string(off) + ":" + std::to_string(scl);
    }
  }
  *out = ref;
  return true;
}

bool AsciiParser::Impl::ParseMetadataBlock() {
  if (!Match(TokenType::OpenParen)) {
    return false;
  }

  PrimSpec* prim = builder_->current();
  if (!prim) {
    AddError("No current prim for metadata");
    return false;
  }

  // Read an arc value that may be a bracketed list or a single value.
  // List-op qualifier applied to an arc list (prepend/append/delete/explicit).
  enum class ArcQual { Explicit, Add, Prepend, Append, Delete, Reorder };

  enum class ArcField { References, Payloads, Inherits, Specializes };
  auto SelectArc = [](PrimSpecMeta& meta,
                      ArcField f) -> std::vector<std::string>& {
    switch (f) {
      case ArcField::References: return meta.references;
      case ArcField::Payloads: return meta.payloads;
      case ArcField::Inherits: return meta.inherits;
      default: return meta.specializes;
    }
  };
  auto SelectEdit = [](ArcListOpEdits& e, ArcField f) -> ArcEdit& {
    switch (f) {
      case ArcField::References: return e.references;
      case ArcField::Payloads: return e.payloads;
      case ArcField::Inherits: return e.inherits;
      default: return e.specializes;
    }
  };

  // Read the bracketed (or single) arc references, then merge into the inline
  // arc vector honoring the list-op qualifier (explicit/bare replaces, prepend
  // front, append/reorder back, delete removes) -- the within-spec effective
  // list. ALSO record the raw qualifier into the spec's ArcEdit (Phase 7 S5),
  // which cross-layer composition (apply_list_ops) and the writer consume. Even
  // a bare empty list (`references = []`) is authored and must replace weaker
  // opinions.
  auto ReadArcList = [this, &SelectArc, &SelectEdit](
                         PrimSpecMeta& meta, ArcField field, ArcQual qual) {
    std::vector<std::string> items;
    if (Check(TokenType::None)) {
      // `references = None`: an explicit-clear list op. Consume the token
      // (leaving it un-consumed desynchronizes the metadata loop) and record
      // an authored empty explicit edit.
      lexer_->next();
      ArcEdit& e0 = SelectEdit(meta.ensure_arc_edits(), field);
      e0 = ArcEdit();
      e0.authored = true;
      SelectArc(meta, field).clear();
      return;
    }
    if (Match(TokenType::OpenBracket)) {
      while (!Check(TokenType::CloseBracket) && !AtEnd()) {
        std::string ref;
        if (!ReadArcRef(&ref)) break;
        items.push_back(ref);
        Match(TokenType::Comma);
      }
      Match(TokenType::CloseBracket);
    } else {
      std::string ref;
      if (ReadArcRef(&ref)) items.push_back(ref);
    }

    // Record the list-op edit first (copies items for non-bare qualifiers).
    ArcEdit& e = SelectEdit(meta.ensure_arc_edits(), field);
    switch (qual) {
      case ArcQual::Explicit:
        e = ArcEdit();  // explicit replaces: is_explicit=true, lists cleared
        e.authored = true;
        break;
      case ArcQual::Prepend:
        e.authored = true;
        e.is_explicit = false;
        e.prepended.insert(e.prepended.end(), items.begin(), items.end());
        break;
      case ArcQual::Append:
        e.authored = true;
        e.is_explicit = false;
        e.appended.insert(e.appended.end(), items.begin(), items.end());
        break;
      case ArcQual::Add:
        e.authored = true;
        e.is_explicit = false;
        e.added.insert(e.added.end(), items.begin(), items.end());
        break;
      case ArcQual::Delete:
        e.authored = true;
        e.is_explicit = false;
        e.deleted.insert(e.deleted.end(), items.begin(), items.end());
        break;
      case ArcQual::Reorder:
        e.authored = true;
        e.is_explicit = false;
        e.ordered.insert(e.ordered.end(), items.begin(), items.end());
        break;
    }

    std::vector<std::string>* target = &SelectArc(meta, field);
    switch (qual) {
      case ArcQual::Explicit:
        *target = std::move(items);
        break;
      case ArcQual::Prepend:
        target->insert(target->begin(), items.begin(), items.end());
        break;
      case ArcQual::Append:
      case ArcQual::Add:
        target->insert(target->end(), items.begin(), items.end());
        break;
      case ArcQual::Reorder:
        ApplyStringListOrder(items, target);
        break;
      case ArcQual::Delete: {
        // Single O(N+M) pass via a hash set of the deleted entries. A per-entry
        // erase(remove()) is O(items * target) = O(N^2) for a `references=[...]`
        // then `delete references=[...]` block with N distinct refs (a ~O(N)
        // text input could hang).
        const std::unordered_set<std::string> del(items.begin(), items.end());
        target->erase(std::remove_if(target->begin(), target->end(),
                                     [&](const std::string& x) {
                                       return del.count(x) != 0;
                                     }),
                      target->end());
        break;
      }
    }
  };

  while (!Check(TokenType::CloseParen) && !AtEnd()) {
    if (Match(TokenType::Semicolon)) continue;
    // A bare (often triple-quoted) string is the prim COMMENT — pxr's only
    // accepted spelling (26.x rejects `comment = "..."` as a non-metadata
    // field; the bare string maps to `comment`, not `doc`).
    if (Check(TokenType::String)) {
      std::string d;
      lexer_->expect(TokenType::String, d);
      prim->meta().comment() = d;
      prim->meta().set_comment_authored();
      continue;
    }
    // Optional list-op qualifier keyword (prepend/append/delete/reorder)
    // precedes the real key, e.g. `prepend references = [...]`.
    ArcQual arc_qual = ArcQual::Explicit;
    if (Match(TokenType::Prepend)) {
      arc_qual = ArcQual::Prepend;
    } else if (Match(TokenType::Append)) {
      arc_qual = ArcQual::Append;
    } else if (Match(TokenType::Add)) {
      arc_qual = ArcQual::Add;
    } else if (Match(TokenType::Delete)) {
      arc_qual = ArcQual::Delete;
    } else if (Match(TokenType::Reorder)) {
      arc_qual = ArcQual::Reorder;
    }

    std::string key;
    if (!lexer_->expect(TokenType::Identifier, key)) {
      AddError("Expected metadata key");
      return false;
    }

    if (!Match(TokenType::Equals)) {
      AddError("Expected '=' after metadata key");
      return false;
    }

    // Handle known prim metadata
    if (key == "active") {
      ParseResult result = ParseValue(*lexer_, TypeId::Bool);
      if (result.success && result.value.as_bool()) {
        builder_->set_active(*result.value.as_bool());
        if (PrimSpec* cur = builder_->current()) {
          cur->meta().active_authored = true;
        }
      }
    } else if (key == "hidden") {
      ParseResult result = ParseValue(*lexer_, TypeId::Bool);
      if (result.success && result.value.as_bool()) {
        builder_->set_hidden(*result.value.as_bool());
        if (PrimSpec* cur = builder_->current()) {
          cur->meta().hidden_authored = true;
        }
      }
    } else if (key == "permission") {
      // Unquoted token (`permission = private`).
      std::string v;
      if (Check(TokenType::Identifier)) lexer_->expect(TokenType::Identifier, v);
      else lexer_->expect(TokenType::String, v);
      if (!v.empty()) prim->meta().permission() = v;
    } else if (key == "doc" || key == "documentation") {
      std::string doc;
      if (lexer_->expect(TokenType::String, doc)) {
        prim->meta().doc() = doc;
        prim->meta().set_doc_authored();
      }
    } else if (key == "comment") {
      std::string v;
      if (lexer_->expect(TokenType::String, v)) {
        prim->meta().comment() = v;
        prim->meta().set_comment_authored();
      }
    } else if (key == "kind") {
      std::string v;
      if (lexer_->expect(TokenType::String, v)) {
        prim->meta().kind() = v;
        prim->meta().setKindAuthored();
      }
    } else if (key == "displayName") {
      std::string v;
      if (lexer_->expect(TokenType::String, v)) {
        prim->meta().displayName() = v;
        prim->meta().setDisplayNameAuthored();
      }
    } else if (key == "displayGroupOrder") {
      ParseResult r = ParseArrayValue(*lexer_, TypeId::String);
      if (r.success && r.value.as_token_array()) {
        prim->meta().editDisplayGroupOrder() = *r.value.as_token_array();
        prim->meta().setDisplayGroupOrderAuthored();
      }
    } else if (key == "instanceable") {
      ParseResult result = ParseValue(*lexer_, TypeId::Bool);
      if (result.success && result.value.as_bool()) {
        prim->meta().instanceable = *result.value.as_bool();
        prim->meta().instanceable_authored = true;
      }
    } else if (key == "relocates") {
      // relocates = { </old/path>: </new/path>, ... } — namespace renames
      // consumed by pcp (SdfRelocates). Relative paths resolve against the
      // owning prim.
      if (prim) prim->meta().setRelocatesAuthored();
      if (Match(TokenType::OpenBrace)) {
        while (!Check(TokenType::CloseBrace) && !AtEnd()) {
          std::string src, dst;
          if (!lexer_->expect(TokenType::PathRef, src)) break;
          if (!Match(TokenType::Colon)) break;
          if (!lexer_->expect(TokenType::PathRef, dst)) break;
          if (prim) {
            auto abs = [&](const std::string& p) {
              if (p.empty() || p[0] == '/') return p;
              return prim->path().str() + "/" + p;
            };
            prim->meta().relocates().emplace_back(abs(src), abs(dst));
          }
          Match(TokenType::Comma);
        }
        Match(TokenType::CloseBrace);
      }
    } else if (key == "apiSchemas") {
      prim->meta().setApiSchemasAuthored();
      std::vector<std::string> schemas;
      if (Check(TokenType::None)) {
        lexer_->next();
      } else if (Match(TokenType::OpenBracket)) {
        while (!Check(TokenType::CloseBracket) && !AtEnd()) {
          // Schema names are authored as quoted strings (`"PhysicsRigidBodyAPI"`);
          // accept bare identifiers too. expect() consumes even on mismatch, so
          // Check the token type first.
          std::string schema;
          if (Check(TokenType::String)
                  ? lexer_->expect(TokenType::String, schema)
                  : lexer_->expect(TokenType::Identifier, schema)) {
            schemas.push_back(schema);
          }
          Match(TokenType::Comma);
        }
        Match(TokenType::CloseBracket);
      } else if (Check(TokenType::String)) {
        // Non-bracketed single value (`prepend apiSchemas = "FooAPI"`): must
        // be consumed here, or the metadata loop's bare-string rule assigns
        // it to the prim DOC (double corruption).
        std::string schema;
        if (lexer_->expect(TokenType::String, schema)) {
          schemas.push_back(schema);
        }
      }
      StringListOpEdits& edits = prim->meta().apiSchemaEdits();
      edits.authored = true;
      if (arc_qual != ArcQual::Explicit && edits.is_explicit) {
        edits = StringListOpEdits();
        edits.authored = true;
      }
      auto append = [&](std::vector<std::string>* dst) {
        dst->insert(dst->end(), schemas.begin(), schemas.end());
      };
      switch (arc_qual) {
        case ArcQual::Explicit:
          edits = StringListOpEdits();
          edits.authored = true;
          edits.is_explicit = true;
          edits.explicit_items = schemas;
          prim->meta().apiSchemasQualifier().clear();
          break;
        case ArcQual::Add:
          edits.is_explicit = false;
          append(&edits.added);
          prim->meta().apiSchemasQualifier() = "append";
          break;
        case ArcQual::Prepend:
          edits.is_explicit = false;
          append(&edits.prepended);
          prim->meta().apiSchemasQualifier() = "prepend";
          break;
        case ArcQual::Append:
          edits.is_explicit = false;
          append(&edits.appended);
          prim->meta().apiSchemasQualifier() = "append";
          break;
        case ArcQual::Delete:
          edits.is_explicit = false;
          append(&edits.deleted);
          prim->meta().apiSchemasQualifier() = "delete";
          break;
        case ArcQual::Reorder:
          edits.is_explicit = false;
          append(&edits.ordered);
          break;
      }
      std::vector<std::string>& applied = prim->meta().apiSchemas();
      applied = edits.is_explicit ? edits.explicit_items : edits.added;
      if (!edits.is_explicit) {
        applied.insert(applied.begin(), edits.prepended.begin(),
                       edits.prepended.end());
        applied.insert(applied.end(), edits.appended.begin(),
                       edits.appended.end());
        std::vector<std::string> reordered;
        for (const std::string& schema : edits.ordered) {
          auto it = std::find(applied.begin(), applied.end(), schema);
          if (it != applied.end()) {
            reordered.push_back(*it);
            applied.erase(it);
          }
        }
        reordered.insert(reordered.end(), applied.begin(), applied.end());
        applied = std::move(reordered);
      }
    } else if (key == "references") {
      ReadArcList(prim->meta(), ArcField::References, arc_qual);
    } else if (key == "payload") {
      ReadArcList(prim->meta(), ArcField::Payloads, arc_qual);
    } else if (key == "inherits") {
      ReadArcList(prim->meta(), ArcField::Inherits, arc_qual);
    } else if (key == "specializes") {
      ReadArcList(prim->meta(), ArcField::Specializes, arc_qual);
    } else if (key == "customData") {
      ParseResult r = ParseDict(*lexer_);
      if (r.success) {
        prim->meta().customData() = std::move(r.value);
        prim->meta().setCustomDataAuthored();
      }
    } else if (key == "assetInfo") {
      ParseResult r = ParseDict(*lexer_);
      if (r.success) {
        prim->meta().assetInfo() = std::move(r.value);
        prim->meta().setAssetInfoAuthored();
      }
    } else if (key == "sdrMetadata") {
      ParseResult r = ParseDict(*lexer_);
      if (r.success) {
        prim->meta().sdrMetadata() = std::move(r.value);
        prim->meta().setSdrMetadataAuthored();
      }
    } else if (key == "clips") {
      ParseResult r = ParseDict(*lexer_);
      if (r.success) {
        prim->meta().clips() = std::move(r.value);
        prim->meta().setClipsAuthored();
      }
    } else if (key == "clipSets") {
      std::vector<std::string> names;
      if (Check(TokenType::None)) {
        lexer_->next();
      } else if (Match(TokenType::OpenBracket)) {
        while (!Check(TokenType::CloseBracket) && !AtEnd()) {
          std::string name;
          if (Check(TokenType::String)
                  ? lexer_->expect(TokenType::String, name)
                  : lexer_->expect(TokenType::Identifier, name)) {
            names.push_back(name);
          }
          Match(TokenType::Comma);
        }
        Match(TokenType::CloseBracket);
      } else {
        std::string name;
        if (Check(TokenType::String)
                ? lexer_->expect(TokenType::String, name)
                : lexer_->expect(TokenType::Identifier, name)) {
          names.push_back(name);
        }
      }

      StringListOpEdits& edits = prim->meta().clipSetEdits();
      edits.authored = true;
      if (arc_qual != ArcQual::Explicit && edits.is_explicit) {
        edits = StringListOpEdits();
        edits.authored = true;
      }
      auto append = [&](std::vector<std::string>* dst) {
        dst->insert(dst->end(), names.begin(), names.end());
      };
      switch (arc_qual) {
        case ArcQual::Explicit:
          edits = StringListOpEdits();
          edits.authored = true;
          edits.is_explicit = true;
          edits.explicit_items = names;
          break;
        case ArcQual::Add:
          edits.is_explicit = false;
          append(&edits.added);
          break;
        case ArcQual::Prepend:
          edits.is_explicit = false;
          append(&edits.prepended);
          break;
        case ArcQual::Append:
          edits.is_explicit = false;
          append(&edits.appended);
          break;
        case ArcQual::Delete:
          edits.is_explicit = false;
          append(&edits.deleted);
          break;
        case ArcQual::Reorder:
          edits.is_explicit = false;
          append(&edits.ordered);
          break;
      }
    } else if (key == "variantSets") {
      // variantSets = ["setName1", "setName2"]  OR a single bare string
      // (`add variantSets = "shadingVariant"`). Declarations only; no body here.
      std::vector<std::string> authored_names;
      auto add_vs = [&](const std::string& vs_name) {
        authored_names.push_back(vs_name);
      };
      if (Match(TokenType::OpenBracket)) {
        while (!Check(TokenType::CloseBracket) && !AtEnd()) {
          // expect() consumes even on mismatch, so dispatch on the peeked type
          // (the old `expect(String) || expect(Identifier)` dropped identifier
          // names by consuming them in the failed String branch).
          std::string vs_name;
          if (Check(TokenType::String)
                  ? lexer_->expect(TokenType::String, vs_name)
                  : lexer_->expect(TokenType::Identifier, vs_name)) {
            add_vs(vs_name);
          }
          Match(TokenType::Comma);
        }
        Match(TokenType::CloseBracket);
      } else {
        std::string vs_name;
        if (Check(TokenType::String)
                ? lexer_->expect(TokenType::String, vs_name)
                : lexer_->expect(TokenType::Identifier, vs_name)) {
          add_vs(vs_name);
        }
      }

      StringListOpEdits& edits = prim->meta().variantSetNameEdits();
      edits.authored = true;
      if (arc_qual != ArcQual::Explicit && edits.is_explicit) {
        edits = StringListOpEdits();
        edits.authored = true;
      }
      auto append = [&](std::vector<std::string>* dst) {
        dst->insert(dst->end(), authored_names.begin(), authored_names.end());
      };
      switch (arc_qual) {
        case ArcQual::Explicit:
          edits = StringListOpEdits();
          edits.authored = true;
          edits.is_explicit = true;
          edits.explicit_items = authored_names;
          break;
        case ArcQual::Add:
          edits.is_explicit = false;
          append(&edits.added);
          break;
        case ArcQual::Prepend:
          edits.is_explicit = false;
          append(&edits.prepended);
          break;
        case ArcQual::Append:
          edits.is_explicit = false;
          append(&edits.appended);
          break;
        case ArcQual::Delete:
          edits.is_explicit = false;
          append(&edits.deleted);
          break;
        case ArcQual::Reorder:
          edits.is_explicit = false;
          append(&edits.ordered);
          break;
      }

      std::vector<std::string> effective = edits.is_explicit
          ? edits.explicit_items
          : edits.added;
      if (!edits.is_explicit) {
        effective.insert(effective.begin(), edits.prepended.begin(),
                         edits.prepended.end());
        effective.insert(effective.end(), edits.appended.begin(),
                         edits.appended.end());
        std::vector<std::string> reordered;
        for (const std::string& name : edits.ordered) {
          auto it = std::find(effective.begin(), effective.end(), name);
          if (it != effective.end()) {
            reordered.push_back(*it);
            effective.erase(it);
          }
        }
        reordered.insert(reordered.end(), effective.begin(), effective.end());
        effective = std::move(reordered);
      }
      std::vector<VariantSetData> old =
          std::move(prim->meta().variantSets());
      std::vector<VariantSetData>& sets = prim->meta().variantSets();
      sets.clear();
      for (const std::string& name : effective) {
        auto it = std::find_if(old.begin(), old.end(),
                               [&](const VariantSetData& set) {
                                 return set.name == name;
                               });
        if (it != old.end()) {
          sets.push_back(std::move(*it));
          old.erase(it);
        } else {
          VariantSetData set;
          set.name = name;
          sets.push_back(std::move(set));
        }
      }
    } else if (key == "variants" || key == "variantSelection") {
      // variants = { string set = "selection"  string set2 = "sel2" }. USD
      // supports a selection per set; record them all in variantSelections()
      // (keeping the legacy single field set to the first for back-compat).
      if (prim) prim->meta().setVariantSelectionsAuthored();
      if (Match(TokenType::OpenBrace)) {
        while (!Check(TokenType::CloseBrace) && !AtEnd()) {
          // Optional leading type name ("string"); the key may be a quoted
          // string or a bare identifier.
          std::string set_name;
          if (Check(TokenType::String)) {
            lexer_->expect(TokenType::String, set_name);
          } else if (Check(TokenType::Identifier)) {
            std::string first;
            lexer_->expect(TokenType::Identifier, first);
            // `string set = ...` -> first is the type name, read the real key.
            if (!Check(TokenType::Equals) &&
                (Check(TokenType::Identifier) || Check(TokenType::String))) {
              if (Check(TokenType::String)) {
                lexer_->expect(TokenType::String, set_name);
              } else {
                lexer_->expect(TokenType::Identifier, set_name);
              }
            } else {
              set_name = first;
            }
          } else {
            break;
          }
          if (!Match(TokenType::Equals)) break;
          std::string sel_name;
          if (!lexer_->expect(TokenType::String, sel_name)) break;
          prim->meta().variantSelections().emplace_back(set_name, sel_name);
          if (prim->meta().variantSelection.empty()) {
            prim->meta().variantSelection = set_name + "=" + sel_name;
          }
          Match(TokenType::Comma);
        }
        Match(TokenType::CloseBrace);
      }
    } else {
      // Unknown (unmodeled) metadata: consume the value structurally, but
      // PRESERVE its raw source text so the writer can re-emit it verbatim
      // (the legacy parser round-trips unknown prim metadata; dropping it
      // loses pipeline-specific opinions like `sceneName`).
      lexer_->peek();  // ensure the value's first token is scanned
      const size_t vstart = lexer_->token_start();
      const bool skipped = SkipValueLike();
      if (skipped) {
        lexer_->peek();  // scan the FOLLOWING token; its start bounds the value
        size_t vend = lexer_->token_start();
        const char* base = lexer_->input_data();
        while (vend > vstart &&
               (base[vend - 1] == ' ' || base[vend - 1] == '\t' ||
                base[vend - 1] == '\r' || base[vend - 1] == '\n')) {
          vend--;
        }
        if (vend > vstart) {
          // A list-op qualifier consumed before the key must ride along or
          // the re-emitted opinion silently changes strength (`prepend
          // weirdKey = [...]` re-emitted bare).
          std::string qual_prefix;
          switch (arc_qual) {
            case ArcQual::Prepend: qual_prefix = "prepend "; break;
            case ArcQual::Append: qual_prefix = "append "; break;
            case ArcQual::Delete: qual_prefix = "delete "; break;
            case ArcQual::Reorder: qual_prefix = "reorder "; break;
            default: break;
          }
          prim->meta().unknownMeta().emplace_back(
              qual_prefix + key, std::string(base + vstart, vend - vstart));
        }
      }
      AddWarning("Unknown prim metadata (preserved): " + key);
    }
  }

  return Match(TokenType::CloseParen);
}

// ============================================================
// AsciiParser public interface
// ============================================================

AsciiParser::AsciiParser(const ParseOptions& options)
    : impl_(std::make_unique<Impl>(NormalizeParseOptions(options))) {}

AsciiParser::~AsciiParser() = default;

AsciiParser::AsciiParser(AsciiParser&&) noexcept = default;
AsciiParser& AsciiParser::operator=(AsciiParser&&) noexcept = default;

bool AsciiParser::Parse(const char* data, size_t length) {
  return impl_->Parse(data, length);
}

bool AsciiParser::ParseOwned(std::string&& data) {
  return impl_->ParseOwned(std::move(data));
}

bool AsciiParser::ParseFile(const char* filename) {
  return impl_->ParseFile(filename);
}

Stage AsciiParser::TakeStage() {
  return impl_->TakeStage();
}

const std::vector<ParseError>& AsciiParser::GetErrors() const {
  return impl_->GetErrors();
}

bool AsciiParser::HasErrors() const {
  return impl_->HasErrors();
}

const std::vector<std::string>& AsciiParser::GetWarnings() const {
  return impl_->GetWarnings();
}

}  // namespace next
}  // namespace lightusd

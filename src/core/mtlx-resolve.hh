// SPDX-License-Identifier: Apache 2.0
// Copyright 2026-Present Light Transport Entertainment Inc.
//
// MaterialX document resolution: strict value-string and name handling,
// scoped attributes (colorspace, fileprefix), XInclude expansion, node
// definition (signature) resolution and the canonical resolved-document dump.
//
// Every function here mirrors a definition of the Lean 4 model in the
// materialx-conformance repository (verif/lean/MtlxSpec); the vectors in
// tests/core/generated/lean-mtlx/*.inc are generated from that model and
// replayed by tests/core/test_mtlx_lean_vectors.cc.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace lightusd {
namespace mtlx {

// ---- Names (MtlxSpec/Name.lean) -------------------------------------------

// MaterialX.Specification.md "MaterialX Names": ASCII letters, digits and
// '_', non-empty, not starting with a digit.
bool IsValidMtlxName(const std::string &name);

// What the reference MaterialX library accepts (`isValidName`): letters,
// digits, '_' and ':' in any position, including the empty string.
bool IsReferenceValidMtlxName(const std::string &name);

// ---- Values (MtlxSpec/Value.lean) -----------------------------------------

enum class MtlxValueKind { None, Int, Bool, Num, Agg, Str, Ints, Nums, Strs };

// A typed value. Float channels are kept as doubles (the correctly rounded
// value of the decimal literal); consumers narrow to float.
struct MtlxTypedValue {
  MtlxValueKind kind = MtlxValueKind::None;
  int64_t i = 0;
  bool b = false;
  double num = 0.0;
  std::vector<double> nums;        // Agg, Nums
  std::vector<int64_t> ints;       // Ints
  std::string str;                 // Str
  std::vector<std::string> strs;   // Strs
};

// Number of float channels of a fixed-size aggregate type (color3, vector2,
// matrix44, ...), or 0.
size_t MtlxTypeArity(const std::string &type);

// Channel stride of a float array type (floatarray, color3array, ...), or 0.
size_t MtlxArrayStride(const std::string &type);

bool IsBuiltinMtlxType(const std::string &type);

// Strict parse of a `value` attribute string of the given MaterialX type.
// Returns false for malformed text and for custom (non-builtin) types.
bool ParseMtlxValue(const std::string &type, const std::string &text,
                    MtlxTypedValue *out);

// The value an unset, unconnected input of this type takes.
bool DefaultMtlxValue(const std::string &type, MtlxTypedValue *out);

// Lexical normal form used by the canonical dump: comma-separated fields,
// each trimmed; string and filename values verbatim.
std::string LexMtlxValue(const std::string &type, const std::string &text);

// ---- Element tree (MtlxSpec/Xml.lean) -------------------------------------

struct MtlxElem {
  std::string tag;
  std::vector<std::pair<std::string, std::string>> attrs;  // document order
  std::vector<MtlxElem> children;

  // First attribute with this key, or nullptr.
  const std::string *Attr(const std::string &key) const;
  std::string AttrOr(const std::string &key,
                     const std::string &fallback = std::string()) const;
  std::string Name() const { return AttrOr("name"); }
  // Replace-or-append; the attribute moves to the end, as in the model.
  void SetAttr(const std::string &key, const std::string &value);
  const MtlxElem *Child(const std::string &name) const;
};

// Parse an MTLX document into its root element. Comments, processing
// instructions, DOCTYPE and text content are skipped; the five predefined
// entities are decoded in attribute values.
bool ParseMtlxXml(const std::string &xml, MtlxElem *root, std::string *err);

// ---- Scoped attributes (MtlxSpec/Doc.lean) --------------------------------

// `chain` is the scope of an element, innermost first (the element itself,
// then each enclosing element out to the root).
using MtlxScope = std::vector<const MtlxElem *>;

// Nearest enclosing value of a hierarchical attribute.
bool ScopedMtlxAttr(const std::string &key, const MtlxScope &chain,
                    std::string *out);

// The color space a color value or image at this scope must be converted
// from; false when unmanaged ("data"/"none" or no colorspace in scope).
bool ActiveMtlxColorSpace(const MtlxScope &chain, std::string *out);

// fileprefix-resolved file name of a `type="filename"` input (chain[0]), with
// interface tokens substituted: "[name]" is replaced by the value of the
// nearest <token name="name"> child of an enclosing element.
bool ResolvedMtlxFilename(const MtlxScope &chain, std::string *out);

bool ParseMtlxVersion(const std::string &text, uint64_t *major,
                      uint64_t *minor);

// ---- XInclude (MtlxSpec/Doc.lean `expand`) --------------------------------

// Maps an href to document text. Returns false when the target is missing.
using MtlxIncludeLoader =
    std::function<bool(const std::string &href, std::string *xml)>;
// `including_href` is empty for the root document and names the document that
// contains this include for recursively included libraries. This lets file
// resolvers anchor nested hrefs to the correct source document.
using MtlxAnchoredIncludeLoader = std::function<bool(
    const std::string &href, const std::string &including_href,
    std::string *xml)>;

// Expand the XIncludes of `root` in place with reference-compatible
// semantics: included children are imported first (in include order, first
// element of a name wins, library root fileprefix/geomprefix/colorspace/
// namespace distributed onto children that lack them, namespaced children
// renamed "ns:name"), then the document's own children are added.
bool ExpandMtlxIncludes(MtlxElem *root, const MtlxIncludeLoader &loader,
                        std::string *err);
bool ExpandMtlxIncludes(MtlxElem *root,
                        const MtlxAnchoredIncludeLoader &loader,
                        std::string *err);

// ---- Node definitions (MtlxSpec/NodeDef.lean) -----------------------------

struct MtlxPort {
  std::string name;
  std::string type;
  bool has_value = false;
  std::string value;
  std::string default_geomprop;
};

struct MtlxNodeDefInfo {
  std::string name;
  std::string node;
  std::string key;  // `node` qualified by the namespace in scope
  bool has_version = false;
  std::string version;
  bool is_default = false;
  std::string target;
  bool has_inherit = false;
  std::string inherit;
  std::vector<MtlxPort> inputs;
  std::vector<MtlxPort> outputs;
};

class MtlxLibrary {
 public:
  // Append every <nodedef> child of `root`.
  void AddFrom(const MtlxElem &root);
  // Append definitions from another library, preserving its document order.
  void AddLibrary(const MtlxLibrary &library);
  // Append one <nodedef> element whose enclosing scope is `outer`.
  void AddNodeDef(const MtlxElem &e, const MtlxScope &outer);

  const std::vector<MtlxNodeDefInfo> &Defs() const { return defs_; }
  const MtlxNodeDefInfo *Find(const std::string &name) const;

  // Inputs including inherited ones, own declarations first.
  std::vector<MtlxPort> ActiveInputs(const MtlxNodeDefInfo &def) const;
  // Outputs including inherited ones, own declarations first.
  std::vector<MtlxPort> ActiveOutputs(const MtlxNodeDefInfo &def) const;
  // The type a node instance of `def` declares: its single (possibly
  // inherited) output's type, or "multioutput".
  std::string OutType(const MtlxNodeDefInfo &def) const;

  // `outer` is the enclosing scope of the node instance (innermost first); it
  // supplies the namespace the category and `nodedef` name are qualified by.

  // Strict resolution: explicit `nodedef`, else the unique definition whose
  // category, version, output type and supplied input types agree.
  const MtlxNodeDefInfo *ResolveStrict(const MtlxElem &node,
                                       const MtlxScope &outer = {}) const;

  // Reference-compatible resolution (`Node::getNodeDef`): the first such
  // definition in document order.
  const MtlxNodeDefInfo *ResolveReference(const MtlxElem &node,
                                          const MtlxScope &outer = {}) const;

  // Reference resolution with rough matches allowed: when no signature
  // agrees, the first definition of the right category, version and output
  // type.
  const MtlxNodeDefInfo *ResolveRough(const MtlxElem &node,
                                      const MtlxScope &outer = {}) const;

 private:
  std::vector<const MtlxNodeDefInfo *> Candidates(const MtlxElem &node,
                                                  const MtlxScope &outer,
                                                  bool exact_only) const;
  const MtlxNodeDefInfo *FindNamed(const MtlxElem &node,
                                   const MtlxScope &outer,
                                   const std::string &name) const;
  std::vector<MtlxPort> ActivePorts(const MtlxNodeDefInfo &def,
                                    bool inputs) const;
  std::vector<MtlxNodeDefInfo> defs_;
};

// Validation result for one nodegraph. Nodes are listed in dependency order,
// so every node feeding another node appears first. The current resolver
// records names while preserving the source DOM for authored values/ports.
struct MtlxResolvedGraph {
  std::vector<std::string> node_order;
};

// Resolve and validate a nodegraph against the nodedef library in `library`.
// This checks node signatures, input bindings, interface types, output types,
// and cycles. `document` supplies sibling nodegraphs referenced by `nodegraph`
// ports; `outer` is the enclosing scope, innermost first.
bool ResolveMtlxGraph(const MtlxElem &document, const MtlxElem &graph,
                      const MtlxLibrary &library, const MtlxScope &outer,
                      MtlxResolvedGraph *resolved, std::string *err);

// Inline one graph-implemented node into a copy of `graph`. This initial
// implementation supports untargeted, unambiguous direct or linked nodegraphs
// and direct node/literal/interface connections. Unsupported forms return false
// without modifying `flattened`.
bool FlattenMtlxGraphCall(const MtlxElem &document, const MtlxElem &graph,
                          const MtlxLibrary &library, const MtlxScope &outer,
                          const std::string &call_name, MtlxElem *flattened,
                          std::string *err);

// Expand graph implementations recursively. Primitive and source-code
// implementations remain nodes. `max_expansions` bounds graph growth; failure
// (including recursive implementations) leaves `flattened` unchanged.
bool FlattenMtlxGraph(const MtlxElem &document, const MtlxElem &graph,
                      const MtlxLibrary &library, const MtlxScope &outer,
                      size_t max_expansions, MtlxElem *flattened,
                      std::string *err);

// Validate every top-level nodegraph and reject cycles between nodegraph
// implementations. `graph_order` is dependency-first.
bool ResolveMtlxGraphs(const MtlxElem &document, const MtlxLibrary &library,
                       std::vector<std::string> *graph_order,
                       std::string *err);

// True when the element is a node instance (not a nodedef, nodegraph,
// typedef, look, ... element).
bool IsMtlxNodeElem(const MtlxElem &e);

// ---- Canonical dump (MtlxSpec/Dump.lean) ----------------------------------

// Line-oriented resolved form of every child of `root`: node definitions
// with active inputs, node graphs and nodes with their resolved definition
// and each input's binding (file prefix and color space applied).
std::string DumpMtlxDocument(const MtlxLibrary &lib, const MtlxElem &root);

}  // namespace mtlx
}  // namespace lightusd

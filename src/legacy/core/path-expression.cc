// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// SdfPathExpression parser + serializer. See path-expression.hh.

#include "core/path-expression.hh"

#include <cctype>
#include <memory>

#if defined(__clang__)
#pragma clang diagnostic push
#if __has_warning("-Wnrvo")
#pragma clang diagnostic ignored "-Wnrvo"
#endif
#endif

namespace lightusd {

namespace {

constexpr size_t kMaxPathExpressionBytes = size_t(1) << 20;  // 1 MiB
constexpr size_t kMaxPathExpressionDepth = 256;
constexpr size_t kMaxPathExpressionNodes = 4096;

inline bool IsBlank(char c) { return c == ' ' || c == '\t'; }

// Characters that can begin a path-expression factor (used to detect an
// implied-union operator: whitespace followed by another factor).
inline bool IsFactorStart(char c) {
  return c == '~' || c == '(' || c == '%' || c == '/' || c == '.' ||
         c == '*' || c == '?' || c == '_' ||
         std::isalnum(static_cast<unsigned char>(c));
}

inline bool SegmentHasWildcard(const std::string &seg) {
  return seg.find('*') != std::string::npos ||
         seg.find('?') != std::string::npos;
}

// Internal parse tree.
struct Node {
  enum class K { Pattern, Ref, Complement, BinOp } kind;
  PathPattern pattern;
  ExpressionReference ref;
  ParsedPathExpression::Op binop{ParsedPathExpression::Op::Union};
  std::unique_ptr<Node> a;  // complement child / binop left
  std::unique_ptr<Node> b;  // binop right
};

using Op = ParsedPathExpression::Op;

// Split a non-stretch segment "text{pred}" into text + predicate(raw, no braces).
PathPatternComponent MakeComponent(const std::string &seg) {
  PathPatternComponent c;
  auto open = seg.find('{');
  if (open != std::string::npos && !seg.empty() && seg.back() == '}') {
    c.text = seg.substr(0, open);
    c.predicate = seg.substr(open + 1, seg.size() - open - 2);
  } else {
    c.text = seg;
  }
  return c;
}

// A scanned pattern element: either a stretch (`//`) or a text segment.
struct ScanElem {
  bool stretch = false;
  std::string text;  // valid when !stretch
};

// Scan a raw pattern token into elements, recognizing `//` as a stretch and a
// single `/` as a separator. Text runs may contain predicate braces `{...}`
// (which may themselves contain '/').
std::vector<ScanElem> ScanPattern(const std::string &raw, bool *absolute) {
  std::vector<ScanElem> elems;
  size_t i = 0;
  const size_t n = raw.size();

  *absolute = (n > 0 && raw[0] == '/');
  if (*absolute) {
    if (n > 1 && raw[1] == '/') {
      elems.push_back(ScanElem{true, ""});  // leading "//" => root + stretch
      i = 2;
    } else {
      i = 1;
    }
  }

  while (i < n) {
    // Read a text run until the next unbraced '/'.
    std::string t;
    int brace = 0;
    while (i < n) {
      char c = raw[i];
      if (c == '{') brace++;
      else if (c == '}' && brace > 0) brace--;
      if (c == '/' && brace == 0) break;
      t.push_back(c);
      i++;
    }
    elems.push_back(ScanElem{false, t});
    // Consume the following separator: "//" => stretch, "/" => plain.
    if (i < n && raw[i] == '/') {
      if (i + 1 < n && raw[i + 1] == '/') {
        elems.push_back(ScanElem{true, ""});
        i += 2;
      } else {
        i += 1;  // plain separator; a trailing single '/' is ignored
      }
    }
  }

  return elems;
}

// Decompose a raw pattern token into a PathPattern.
PathPattern DecomposePattern(const std::string &raw) {
  PathPattern p;
  if (raw.empty()) {
    return p;
  }

  bool absolute = false;
  std::vector<ScanElem> elems = ScanPattern(raw, &absolute);

  // Detect a trailing property on the final text element: "name.prop"
  // (not the relative tokens "." / ".."), outside a predicate.
  std::string property;
  if (!elems.empty() && !elems.back().stretch) {
    std::string &last = elems.back().text;
    if (last != "." && last != ".." && last.find('{') == std::string::npos) {
      auto dot = last.find('.');
      if (dot != std::string::npos && dot > 0 && dot + 1 < last.size()) {
        property = last.substr(dot + 1);
        last = last.substr(0, dot);
      }
    }
  }

  // Fold leading literal (non-wildcard, non-predicate, non-stretch) elements
  // into the prefix path; the remainder become components.
  std::string prefix = absolute ? "/" : "";
  size_t i = 0;
  for (; i < elems.size(); i++) {
    const ScanElem &e = elems[i];
    const bool isLiteral = !e.stretch && !SegmentHasWildcard(e.text) &&
                           e.text.find('{') == std::string::npos;
    if (isLiteral) {
      if (prefix.empty() || prefix == "/") {
        prefix += e.text;
      } else {
        prefix += "/" + e.text;
      }
    } else {
      break;
    }
  }

  p.prefix = prefix;
  for (; i < elems.size(); i++) {
    if (elems[i].stretch) {
      p.components.push_back(PathPatternComponent{});  // stretch
    } else {
      p.components.push_back(MakeComponent(elems[i].text));
    }
  }

  if (!property.empty()) {
    p.match_property = true;
    p.property_name = property;
    PathPatternComponent c;
    c.text = property;
    p.components.push_back(c);
  }

  return p;
}

class Parser {
 public:
  explicit Parser(const std::string &s) : s_(s) {}

  std::unique_ptr<Node> ParseTop() {
    if (s_.size() > kMaxPathExpressionBytes) {
      Fail("Path expression is too large");
      return nullptr;
    }
    SkipBlanks();
    if (Eof()) {
      return nullptr;  // empty expression (valid)
    }
    auto n = ParseExpr(0);
    if (failed_) return nullptr;
    SkipBlanks();
    if (!Eof()) {
      Fail("Unexpected trailing characters in path expression");
      return nullptr;
    }
    return std::unique_ptr<Node>(std::move(n));
  }

  bool failed() const { return failed_; }
  const std::string &error() const { return err_; }

 private:
  bool Eof() const { return pos_ >= s_.size(); }
  char Peek() const { return Eof() ? '\0' : s_[pos_]; }
  void SkipBlanks() {
    while (!Eof() && IsBlank(s_[pos_])) pos_++;
  }
  void Fail(const std::string &m) {
    if (!failed_) {
      failed_ = true;
      err_ = m;
    }
  }

  bool NoteNode() {
    if (++nodes_ > kMaxPathExpressionNodes) {
      Fail("Path expression has too many nodes");
      return false;
    }
    return true;
  }

  std::unique_ptr<Node> ParseExpr(size_t depth) {
    auto left = ParseFactor(depth);
    if (failed_) return nullptr;
    while (true) {
      Op op;
      if (!ParseOperator(&op)) break;
      auto right = ParseFactor(depth);
      if (failed_) return nullptr;
      if (!NoteNode()) return nullptr;
      auto bin = std::make_unique<Node>();
      bin->kind = Node::K::BinOp;
      bin->binop = op;
      bin->a = std::move(left);
      bin->b = std::move(right);
      left = std::move(bin);
    }
    return std::unique_ptr<Node>(std::move(left));
  }

  std::unique_ptr<Node> ParseFactor(size_t depth) {
    SkipBlanks();
    bool complement = false;
    if (Peek() == '~') {
      complement = true;
      pos_++;
      SkipBlanks();
    }
    auto atom = ParseAtom(depth);
    if (failed_) return nullptr;
    if (complement) {
      if (!NoteNode()) return nullptr;
      auto c = std::make_unique<Node>();
      c->kind = Node::K::Complement;
      c->a = std::move(atom);
      return c;
    }
    return std::unique_ptr<Node>(std::move(atom));
  }

  std::unique_ptr<Node> ParseAtom(size_t depth) {
    SkipBlanks();
    char c = Peek();
    if (c == '(') {
      if (depth >= kMaxPathExpressionDepth) {
        Fail("Path expression nesting is too deep");
        return nullptr;
      }
      pos_++;
      SkipBlanks();
      auto e = ParseExpr(depth + 1);
      if (failed_) return nullptr;
      SkipBlanks();
      if (Peek() != ')') {
        Fail("Expected ')' in path expression");
        return nullptr;
      }
      pos_++;
      return std::unique_ptr<Node>(std::move(e));
    }
    if (c == '%') {
      return ParseRef();
    }
    return ParsePattern();
  }

  std::unique_ptr<Node> ParseRef() {
    // '%' already at Peek().
    pos_++;  // consume '%'
    // Weaker reference '%_'
    std::string body;
    while (!Eof()) {
      char c = Peek();
      if (IsBlank(c) || c == '+' || c == '&' || c == '-' || c == '~' ||
          c == '(' || c == ')') {
        break;
      }
      body.push_back(c);
      pos_++;
    }
    ExpressionReference ref;
    if (body == "_") {
      ref.name = "_";
    } else {
      auto colon = body.rfind(':');
      if (colon != std::string::npos) {
        ref.path = body.substr(0, colon);
        ref.name = body.substr(colon + 1);
      } else {
        ref.path = body;
      }
    }
    auto n = std::make_unique<Node>();
    n->kind = Node::K::Ref;
    n->ref = std::move(ref);
    if (!NoteNode()) return nullptr;
    return std::unique_ptr<Node>(std::move(n));
  }

  std::unique_ptr<Node> ParsePattern() {
    std::string raw;
    while (!Eof()) {
      char c = Peek();
      if (IsBlank(c) || c == '+' || c == '&' || c == '-' || c == '~' ||
          c == '(' || c == ')') {
        break;
      }
      if (c == '{') {
        int depth = 0;
        while (!Eof()) {
          char cc = Peek();
          raw.push_back(cc);
          pos_++;
          if (cc == '{') depth++;
          else if (cc == '}') {
            depth--;
            if (depth == 0) break;
          }
        }
        continue;
      }
      raw.push_back(c);
      pos_++;
    }
    if (raw.empty()) {
      Fail("Expected a path pattern in path expression");
      return nullptr;
    }
    auto n = std::make_unique<Node>();
    n->kind = Node::K::Pattern;
    n->pattern = DecomposePattern(raw);
    if (!NoteNode()) return nullptr;
    return std::unique_ptr<Node>(std::move(n));
  }

  // Per OpenUSD grammar: OptSpaced<'+'|'&'|'-'> | implied-union(plus<blank>).
  bool ParseOperator(Op *op) {
    size_t save = pos_;
    size_t blanks = 0;
    while (!Eof() && IsBlank(s_[pos_])) {
      pos_++;
      blanks++;
    }
    if (Eof()) {
      pos_ = save;
      return false;
    }
    char c = Peek();
    if (c == '+' || c == '&' || c == '-') {
      pos_++;
      SkipBlanks();
      *op = (c == '+') ? Op::Union
                       : (c == '&') ? Op::Intersection : Op::Difference;
      return true;
    }
    if (blanks >= 1 && IsFactorStart(c)) {
      *op = Op::ImpliedUnion;
      return true;
    }
    pos_ = save;
    return false;
  }

  const std::string &s_;
  size_t pos_ = 0;
  size_t nodes_ = 0;
  bool failed_ = false;
  std::string err_;
};

// Flatten the parse tree into prefix-order ops + left-to-right patterns/refs.
void Flatten(const Node *n, std::vector<Op> *ops,
             std::vector<PathPattern> *patterns,
             std::vector<ExpressionReference> *refs) {
  if (!n) return;
  std::vector<const Node *> stack;
  stack.push_back(n);
  while (!stack.empty()) {
    const Node *cur = stack.back();
    stack.pop_back();
    if (!cur) continue;
    switch (cur->kind) {
      case Node::K::Pattern:
        ops->push_back(Op::Pattern);
        patterns->push_back(cur->pattern);
        break;
      case Node::K::Ref:
        ops->push_back(Op::ExpressionRef);
        refs->push_back(cur->ref);
        break;
      case Node::K::Complement:
        ops->push_back(Op::Complement);
        stack.push_back(cur->a.get());
        break;
      case Node::K::BinOp:
        ops->push_back(cur->binop);
        stack.push_back(cur->b.get());
        stack.push_back(cur->a.get());
        break;
    }
  }
}

}  // namespace

std::string PathPattern::GetText() const {
  // Mirrors SdfPathPattern::GetText().
  std::string result = prefix;
  const bool prefixIsAbsRoot = (prefix == "/");
  for (size_t i = 0, end = components.size(); i != end; ++i) {
    const PathPatternComponent &c = components[i];
    if (c.is_stretch()) {
      result += (i == 0 && prefixIsAbsRoot) ? "/" : "//";
      continue;
    }
    if ((i + 1 == end) && match_property) {
      result.push_back('.');
    } else if (!result.empty() && result.back() != '/') {
      result.push_back('/');
    }
    result += c.text;
    if (!c.predicate.empty()) {
      result += "{" + c.predicate + "}";
    }
  }
  return result;
}

std::string ExpressionReference::GetText() const {
  if (is_weaker()) {
    return "%_";
  }
  std::string r = "%" + path;
  if (!name.empty()) {
    r += ":" + name;
  }
  return r;
}

namespace {

// Operator text, mirroring SdfPathExpression::GetOpText().
const char *OpText(Op op) {
  switch (op) {
    case Op::Complement: return "~";
    case Op::ImpliedUnion: return " ";
    case Op::Union: return " + ";
    case Op::Intersection: return " & ";
    case Op::Difference: return " - ";
    case Op::Pattern:
    case Op::ExpressionRef: return "";
  }
  return "";
}

// Recursively serialize from the prefix-op stream.
struct Serializer {
  const std::vector<Op> &ops;
  const std::vector<PathPattern> &patterns;
  const std::vector<ExpressionReference> &refs;
  size_t oi = 0, pi = 0, ri = 0;

  std::string Emit() {
    if (oi >= ops.size()) return "";
    Op op = ops[oi++];
    switch (op) {
      case Op::Pattern:
        return patterns[pi++].GetText();
      case Op::ExpressionRef:
        return refs[ri++].GetText();
      case Op::Complement: {
        // A factor accepts at most one `~`, so a complement-of-complement must
        // be grouped: `~(~A)`, never `~~A` (which would not re-parse).
        std::string child = EmitOperand(/*parenthesize_complement=*/true);
        return std::string("~") + child;
      }
      default: {
        std::string lhs = EmitOperand();
        std::string rhs = EmitOperand();
        return lhs + OpText(op) + rhs;
      }
    }
  }

  // Emit a sub-expression, parenthesizing a binary sub-op (lower/equal
  // precedence than the surrounding op; operands bind tighter). When
  // `parenthesize_complement` is set, a complement sub-op is grouped too.
  std::string EmitOperand(bool parenthesize_complement = false) {
    if (oi >= ops.size()) return "";
    Op next = ops[oi];
    const bool isBin = (next == Op::ImpliedUnion || next == Op::Union ||
                        next == Op::Intersection || next == Op::Difference);
    const bool needGroup =
        isBin || (parenthesize_complement && next == Op::Complement);
    std::string sub = Emit();
    if (needGroup) {
      return "(" + sub + ")";
    }
    return std::string(std::move(sub));
  }
};

}  // namespace

std::string ParsedPathExpression::GetText() const {
  if (ops_.empty()) return "";
  Serializer ser{ops_, patterns_, refs_};
  return ser.Emit();
}

ParsedPathExpression ParsedPathExpression::Parse(const std::string &text,
                                                 std::string *err) {
  ParsedPathExpression out;
  Parser parser(text);
  std::unique_ptr<Node> root = parser.ParseTop();
  if (parser.failed()) {
    out.valid_ = false;
    out.parse_error_ = parser.error();
    if (err) *err = parser.error();
    return out;
  }
  // Empty expression: valid, no ops.
  if (root) {
    Flatten(root.get(), &out.ops_, &out.patterns_, &out.refs_);
  }
  out.valid_ = true;
  return out;
}

ParsedPathExpression ParsePathExpression(const value::PathExpression &expr,
                                         std::string *err) {
  return ParsedPathExpression::Parse(expr.GetText(), err);
}

}  // namespace lightusd

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

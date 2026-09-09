// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// SdfVariableExpression-style expression language for composition
// (asset paths, sublayer paths, variant selections):
//
//   expr     := value
//   value    := '${' NAME '}' | quoted-string | int64 | 'True' | 'False'
//             | 'None' | '[' value (',' value)* ']' | ident '(' args ')'
//   functions: if, and, or, not, eq, neq, lt, leq, gt, geq, contains, at,
//              len, defined
//
// Quoted strings interpolate `${VAR}` and honor backslash escapes. Variables
// whose values are themselves backtick expressions evaluate recursively with
// cycle detection. Parse nesting and variable expansion are hard-capped
// (malformed-input DoS hardening, same spirit as the unknownMeta decoder).

#include "expression-variables.hh"

#include <cerrno>
#include <cstdlib>

namespace lightusd {
namespace next {

namespace {

const std::string* StringValue(const Value& value) {
  if (const std::string* s = value.as_string()) return s;
  if (const std::string* s = value.as_token()) return s;
  return value.as_asset_path();
}

constexpr int kMaxParseDepth = 64;
constexpr int kMaxVariableExpansionDepth = 32;

// Typed evaluation result. List elements are restricted to scalar kinds.
struct ExprValue {
  enum class Kind { None, Bool, Int, String, List };
  Kind kind = Kind::None;
  bool b = false;
  int64_t i = 0;
  std::string s;
  std::vector<ExprValue> list;

  static ExprValue MakeNone() { return ExprValue(); }
  static ExprValue MakeBool(bool v) {
    ExprValue e;
    e.kind = Kind::Bool;
    e.b = v;
    return e;
  }
  static ExprValue MakeInt(int64_t v) {
    ExprValue e;
    e.kind = Kind::Int;
    e.i = v;
    return e;
  }
  static ExprValue MakeString(std::string v) {
    ExprValue e;
    e.kind = Kind::String;
    e.s = std::move(v);
    return e;
  }
  const char* kind_name() const {
    switch (kind) {
      case Kind::None: return "None";
      case Kind::Bool: return "bool";
      case Kind::Int: return "int";
      case Kind::String: return "string";
      case Kind::List: return "list";
    }
    return "?";
  }
};

class ExpressionEvaluator {
 public:
  ExpressionEvaluator(const Dict* vars) : vars_(vars) {}

  // Evaluate a full backtick-stripped expression body.
  bool EvalBody(const std::string& body, ExprValue* out) {
    text_ = &body;
    pos_ = 0;
    if (!ParseValue(out, 0)) return false;
    SkipWs();
    if (pos_ != body.size()) {
      return Fail("unexpected trailing characters in expression");
    }
    return true;
  }

  const std::string& error() const { return error_; }

  // Recursive evaluation of a variable whose value is itself an expression.
  bool EvalVariableExpression(const std::string& name,
                              const std::string& expr_text, ExprValue* out) {
    for (const std::string& active : var_stack_) {
      if (active == name) {
        return Fail("expression variable cycle detected at `" + name + "`");
      }
    }
    if (var_stack_.size() >=
        static_cast<size_t>(kMaxVariableExpansionDepth)) {
      return Fail("expression variable expansion depth exceeded");
    }
    var_stack_.push_back(name);
    // Save/restore the outer parse state around the nested evaluation.
    const std::string* saved_text = text_;
    size_t saved_pos = pos_;
    const std::string expr = expr_text.substr(1, expr_text.size() - 2);
    const bool ok = EvalBody(expr, out);
    text_ = saved_text;
    pos_ = saved_pos;
    var_stack_.pop_back();
    return ok;
  }

 private:
  bool Fail(const std::string& message) {
    if (error_.empty()) error_ = message;
    return false;
  }

  void SkipWs() {
    while (pos_ < text_->size() &&
           ((*text_)[pos_] == ' ' || (*text_)[pos_] == '\t' ||
            (*text_)[pos_] == '\n' || (*text_)[pos_] == '\r')) {
      ++pos_;
    }
  }

  bool ParseValue(ExprValue* out, int depth) {
    if (depth > kMaxParseDepth) return Fail("expression nesting too deep");
    SkipWs();
    if (pos_ >= text_->size()) return Fail("expected a value");
    const char c = (*text_)[pos_];
    if (c == '$') return ParseVariable(out);
    if (c == '\'' || c == '"') return ParseString(out);
    if (c == '[') return ParseList(out, depth);
    if (c == '-' || (c >= '0' && c <= '9')) return ParseInt(out);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
      return ParseKeywordOrCall(out, depth);
    }
    return Fail(std::string("unexpected character `") + c +
                "` in expression");
  }

  bool ParseVariable(ExprValue* out) {
    if (text_->compare(pos_, 2, "${") != 0) {
      return Fail("expected `${` variable reference");
    }
    const size_t end = text_->find('}', pos_ + 2);
    if (end == std::string::npos) {
      return Fail("unterminated variable reference");
    }
    const std::string name = text_->substr(pos_ + 2, end - pos_ - 2);
    pos_ = end + 1;
    return LookupVariable(name, out);
  }

  bool LookupVariable(const std::string& name, ExprValue* out) {
    const Value* v = vars_ ? vars_->find(name) : nullptr;
    if (!v) {
      return Fail("undefined expression variable `" + name + "`");
    }
    if (const std::string* s = StringValue(*v)) {
      if (s->size() >= 2 && s->front() == '`' && s->back() == '`') {
        return EvalVariableExpression(name, *s, out);
      }
      *out = ExprValue::MakeString(*s);
      return true;
    }
    if (const bool* b = v->as_bool()) {
      *out = ExprValue::MakeBool(*b);
      return true;
    }
    if (const int32_t* i = v->as_int()) {
      *out = ExprValue::MakeInt(*i);
      return true;
    }
    if (const int64_t* i = v->as_int64()) {
      *out = ExprValue::MakeInt(*i);
      return true;
    }
    if (const uint32_t* u = v->as_uint()) {
      *out = ExprValue::MakeInt(static_cast<int64_t>(*u));
      return true;
    }
    if (const std::vector<std::string>* a = v->as_token_array()) {
      out->kind = ExprValue::Kind::List;
      out->list.clear();
      for (const std::string& e : *a) out->list.push_back(ExprValue::MakeString(e));
      return true;
    }
    if (const std::vector<int32_t>* a = v->as_int_array()) {
      out->kind = ExprValue::Kind::List;
      out->list.clear();
      for (int32_t e : *a) out->list.push_back(ExprValue::MakeInt(e));
      return true;
    }
    if (const std::vector<int64_t>* a = v->as_int64_array()) {
      out->kind = ExprValue::Kind::List;
      out->list.clear();
      for (int64_t e : *a) out->list.push_back(ExprValue::MakeInt(e));
      return true;
    }
    return Fail("expression variable `" + name +
                "` has an unsupported value type");
  }

  bool ParseString(ExprValue* out) {
    const char quote = (*text_)[pos_];
    ++pos_;
    std::string result;
    while (pos_ < text_->size() && (*text_)[pos_] != quote) {
      const char c = (*text_)[pos_];
      if (c == '\\') {
        if (pos_ + 1 >= text_->size()) return Fail("dangling escape in string");
        result += (*text_)[pos_ + 1];
        pos_ += 2;
        continue;
      }
      if (c == '$' && text_->compare(pos_, 2, "${") == 0) {
        const size_t end = text_->find('}', pos_ + 2);
        if (end == std::string::npos) {
          return Fail("unterminated variable reference in string");
        }
        const std::string name = text_->substr(pos_ + 2, end - pos_ - 2);
        ExprValue v;
        if (!LookupVariable(name, &v)) return false;
        if (v.kind != ExprValue::Kind::String) {
          return Fail("interpolated variable `" + name +
                      "` is not string-valued");
        }
        result += v.s;
        pos_ = end + 1;
        continue;
      }
      result += c;
      ++pos_;
    }
    if (pos_ >= text_->size()) return Fail("unterminated string literal");
    ++pos_;  // closing quote
    *out = ExprValue::MakeString(std::move(result));
    return true;
  }

  bool ParseInt(ExprValue* out) {
    const size_t start = pos_;
    if ((*text_)[pos_] == '-') ++pos_;
    size_t digits = 0;
    while (pos_ < text_->size() && (*text_)[pos_] >= '0' &&
           (*text_)[pos_] <= '9') {
      ++pos_;
      ++digits;
    }
    if (!digits) return Fail("malformed integer literal");
    const std::string token = text_->substr(start, pos_ - start);
    errno = 0;
    char* endp = nullptr;
    const long long v = std::strtoll(token.c_str(), &endp, 10);
    if (errno == ERANGE || !endp || *endp != '\0') {
      return Fail("integer literal out of int64 range: " + token);
    }
    *out = ExprValue::MakeInt(static_cast<int64_t>(v));
    return true;
  }

  bool ParseList(ExprValue* out, int depth) {
    ++pos_;  // '['
    out->kind = ExprValue::Kind::List;
    out->list.clear();
    SkipWs();
    if (pos_ < text_->size() && (*text_)[pos_] == ']') {
      ++pos_;
      return true;
    }
    for (;;) {
      ExprValue element;
      if (!ParseValue(&element, depth + 1)) return false;
      if (element.kind == ExprValue::Kind::List) {
        return Fail("lists cannot be nested");
      }
      out->list.push_back(std::move(element));
      SkipWs();
      if (pos_ >= text_->size()) return Fail("unterminated list literal");
      if ((*text_)[pos_] == ',') {
        ++pos_;
        continue;
      }
      if ((*text_)[pos_] == ']') {
        ++pos_;
        return true;
      }
      return Fail("expected `,` or `]` in list literal");
    }
  }

  bool ParseKeywordOrCall(ExprValue* out, int depth) {
    const size_t start = pos_;
    while (pos_ < text_->size() &&
           (((*text_)[pos_] >= 'a' && (*text_)[pos_] <= 'z') ||
            ((*text_)[pos_] >= 'A' && (*text_)[pos_] <= 'Z') ||
            ((*text_)[pos_] >= '0' && (*text_)[pos_] <= '9') ||
            (*text_)[pos_] == '_')) {
      ++pos_;
    }
    const std::string ident = text_->substr(start, pos_ - start);
    if (ident == "True") {
      *out = ExprValue::MakeBool(true);
      return true;
    }
    if (ident == "False") {
      *out = ExprValue::MakeBool(false);
      return true;
    }
    if (ident == "None") {
      *out = ExprValue::MakeNone();
      return true;
    }
    SkipWs();
    if (pos_ >= text_->size() || (*text_)[pos_] != '(') {
      return Fail("unknown expression keyword `" + ident + "`");
    }
    ++pos_;  // '('
    std::vector<ExprValue> args;
    SkipWs();
    if (pos_ < text_->size() && (*text_)[pos_] == ')') {
      ++pos_;
    } else {
      for (;;) {
        ExprValue arg;
        if (!ParseValue(&arg, depth + 1)) return false;
        args.push_back(std::move(arg));
        SkipWs();
        if (pos_ >= text_->size()) return Fail("unterminated function call");
        if ((*text_)[pos_] == ',') {
          ++pos_;
          continue;
        }
        if ((*text_)[pos_] == ')') {
          ++pos_;
          break;
        }
        return Fail("expected `,` or `)` in function call");
      }
    }
    return EvalFunction(ident, args, out);
  }

  static bool SameScalarKind(const ExprValue& a, const ExprValue& b) {
    return a.kind == b.kind && a.kind != ExprValue::Kind::List;
  }

  static bool ScalarEquals(const ExprValue& a, const ExprValue& b) {
    switch (a.kind) {
      case ExprValue::Kind::None: return true;
      case ExprValue::Kind::Bool: return a.b == b.b;
      case ExprValue::Kind::Int: return a.i == b.i;
      case ExprValue::Kind::String: return a.s == b.s;
      case ExprValue::Kind::List: return false;
    }
    return false;
  }

  bool EvalFunction(const std::string& name,
                    const std::vector<ExprValue>& args, ExprValue* out) {
    auto arity = [&](size_t lo, size_t hi) {
      if (args.size() < lo || args.size() > hi) {
        return Fail("`" + name + "` expects " + std::to_string(lo) +
                    (hi == lo ? "" : ".." + std::to_string(hi)) +
                    " argument(s)");
      }
      return true;
    };
    auto require_bool = [&](const ExprValue& v) {
      if (v.kind != ExprValue::Kind::Bool) {
        return Fail("`" + name + "` requires boolean arguments, got " +
                    std::string(v.kind_name()));
      }
      return true;
    };
    if (name == "if") {
      if (!arity(2, 3)) return false;
      if (!require_bool(args[0])) return false;
      if (args[0].b) *out = args[1];
      else if (args.size() == 3) *out = args[2];
      else *out = ExprValue::MakeNone();
      return true;
    }
    if (name == "and" || name == "or") {
      if (args.size() < 2) return Fail("`" + name + "` expects 2+ arguments");
      bool acc = name == "and";
      for (const ExprValue& a : args) {
        if (!require_bool(a)) return false;
        acc = name == "and" ? (acc && a.b) : (acc || a.b);
      }
      *out = ExprValue::MakeBool(acc);
      return true;
    }
    if (name == "not") {
      if (!arity(1, 1)) return false;
      if (!require_bool(args[0])) return false;
      *out = ExprValue::MakeBool(!args[0].b);
      return true;
    }
    if (name == "eq" || name == "neq") {
      if (!arity(2, 2)) return false;
      if (!SameScalarKind(args[0], args[1])) {
        return Fail("`" + name + "` requires two comparable values of the "
                    "same type");
      }
      const bool equal = ScalarEquals(args[0], args[1]);
      *out = ExprValue::MakeBool(name == "eq" ? equal : !equal);
      return true;
    }
    if (name == "lt" || name == "leq" || name == "gt" || name == "geq") {
      if (!arity(2, 2)) return false;
      int cmp = 0;
      if (args[0].kind == ExprValue::Kind::Int &&
          args[1].kind == ExprValue::Kind::Int) {
        cmp = args[0].i < args[1].i ? -1 : (args[0].i > args[1].i ? 1 : 0);
      } else if (args[0].kind == ExprValue::Kind::String &&
                 args[1].kind == ExprValue::Kind::String) {
        cmp = args[0].s.compare(args[1].s);
        cmp = cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
      } else {
        return Fail("`" + name + "` requires two ints or two strings");
      }
      bool r = false;
      if (name == "lt") r = cmp < 0;
      else if (name == "leq") r = cmp <= 0;
      else if (name == "gt") r = cmp > 0;
      else r = cmp >= 0;
      *out = ExprValue::MakeBool(r);
      return true;
    }
    if (name == "contains") {
      if (!arity(2, 2)) return false;
      if (args[0].kind == ExprValue::Kind::List) {
        for (const ExprValue& e : args[0].list) {
          if (SameScalarKind(e, args[1]) && ScalarEquals(e, args[1])) {
            *out = ExprValue::MakeBool(true);
            return true;
          }
        }
        *out = ExprValue::MakeBool(false);
        return true;
      }
      if (args[0].kind == ExprValue::Kind::String &&
          args[1].kind == ExprValue::Kind::String) {
        *out = ExprValue::MakeBool(args[0].s.find(args[1].s) !=
                                   std::string::npos);
        return true;
      }
      return Fail("`contains` requires (list, value) or (string, string)");
    }
    if (name == "at") {
      if (!arity(2, 2)) return false;
      if (args[1].kind != ExprValue::Kind::Int) {
        return Fail("`at` index must be an int");
      }
      int64_t idx = args[1].i;
      if (args[0].kind == ExprValue::Kind::List) {
        const int64_t n = static_cast<int64_t>(args[0].list.size());
        if (idx < 0) idx += n;
        if (idx < 0 || idx >= n) return Fail("`at` index out of range");
        *out = args[0].list[static_cast<size_t>(idx)];
        return true;
      }
      if (args[0].kind == ExprValue::Kind::String) {
        const int64_t n = static_cast<int64_t>(args[0].s.size());
        if (idx < 0) idx += n;
        if (idx < 0 || idx >= n) return Fail("`at` index out of range");
        *out = ExprValue::MakeString(
            std::string(1, args[0].s[static_cast<size_t>(idx)]));
        return true;
      }
      return Fail("`at` requires a list or string");
    }
    if (name == "len") {
      if (!arity(1, 1)) return false;
      if (args[0].kind == ExprValue::Kind::List) {
        *out = ExprValue::MakeInt(static_cast<int64_t>(args[0].list.size()));
        return true;
      }
      if (args[0].kind == ExprValue::Kind::String) {
        *out = ExprValue::MakeInt(static_cast<int64_t>(args[0].s.size()));
        return true;
      }
      return Fail("`len` requires a list or string");
    }
    if (name == "defined") {
      if (args.empty()) return Fail("`defined` expects 1+ arguments");
      bool all = true;
      for (const ExprValue& a : args) {
        if (a.kind != ExprValue::Kind::String) {
          return Fail("`defined` arguments must be variable-name strings");
        }
        all = all && vars_ && vars_->find(a.s) != nullptr;
      }
      *out = ExprValue::MakeBool(all);
      return true;
    }
    return Fail("unknown expression function `" + name + "`");
  }

  const Dict* vars_ = nullptr;
  const std::string* text_ = nullptr;
  size_t pos_ = 0;
  std::string error_;
  std::vector<std::string> var_stack_;
};

}  // namespace

Value ComposeExpressionVariables(const Value& weaker, const Value& stronger) {
  Value result = Value::MakeDictionary();
  Dict* out = result.as_dictionary();
  if (const Dict* d = weaker.as_dictionary()) {
    for (const auto& entry : d->entries()) out->set(entry.first, entry.second);
  }
  if (const Dict* d = stronger.as_dictionary()) {
    for (const auto& entry : d->entries()) out->set(entry.first, entry.second);
  }
  return result;
}

ExpressionEvaluation EvaluateAssetPathExpression(const std::string& path,
                                                 const Value& variables) {
  ExpressionEvaluation result;
  result.value = path;
  if (path.size() < 2 || path.front() != '`' || path.back() != '`') {
    return result;
  }
  result.is_expression = true;

  ExpressionEvaluator evaluator(variables.as_dictionary());
  ExprValue value;
  const std::string expr_body = path.substr(1, path.size() - 2);
  if (!evaluator.EvalBody(expr_body, &value)) {
    result.success = false;
    result.error = evaluator.error() + " in: " + path;
    return result;
  }
  switch (value.kind) {
    case ExprValue::Kind::String:
      result.value = std::move(value.s);
      return result;
    case ExprValue::Kind::None:
      // "No opinion": the caller skips the arc / selection.
      result.is_none = true;
      result.value.clear();
      return result;
    default:
      result.success = false;
      result.error = std::string("expression evaluated to ") +
                     value.kind_name() + " where a string is required: " +
                     path;
      return result;
  }
}

}  // namespace next
}  // namespace lightusd

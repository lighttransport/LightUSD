// SPDX-License-Identifier: Apache 2.0
// Copyright present, Light Transport Entertainment, Inc.
//
// value::Value pretty-print dispatch — split out of value-pprint.cc.
// Holds pprint_value(), the big switch-on-type_id that renders a value::Value:
// base value types via operator<<, 1D arrays via print_1d_array, and ~60 schema
// prim types (GeomMesh/Material/Shader/...) via to_string(). The schema arms are
// the only thing in value-pprint.cc that needed usdGeom.hh/usdLux.hh/core/prim.hh;
// isolating them lets value-pprint.cc (operator<< + to_string for value types)
// drop those heavy schema headers. operator<< / to_string are declared in
// value-pprint.hh / pprinter.hh and resolved cross-TU.
#include "value-pprint.hh"

#include <sstream>

#include "pprinter.hh"
#include "core/prim.hh"
#include "str-util.hh"
#include "value-types.hh"
#include "array-edit.hh"  // value::ArrayEdit (VtArrayEdit)

#include "common-macros.inc"

namespace lightusd {
namespace value {

// Print literal element `idx` of a VtArrayEdit's type-erased literals array,
// for the OpenUSD `edit [...]` textual form.
static void PrintArrayEditLiteral(std::ostream& os, const Value& lits,
                                  int64_t idx) {
#define PE_NUM(T)                                                  \
  if (auto* p = lits.as<std::vector<T>>()) {                       \
    if (idx >= 0 && static_cast<size_t>(idx) < p->size())          \
      os << (*p)[static_cast<size_t>(idx)];                        \
    else                                                           \
      os << "<?>";                                                 \
    return;                                                        \
  }
  PE_NUM(int32_t) PE_NUM(int64_t) PE_NUM(uint32_t) PE_NUM(uint64_t)
  PE_NUM(float) PE_NUM(double)
  PE_NUM(value::float2) PE_NUM(value::float3) PE_NUM(value::float4)
  PE_NUM(value::double2) PE_NUM(value::double3) PE_NUM(value::double4)
  PE_NUM(value::int2) PE_NUM(value::int3) PE_NUM(value::int4)
#undef PE_NUM
  if (auto* p = lits.as<std::vector<std::string>>()) {
    if (idx >= 0 && static_cast<size_t>(idx) < p->size())
      os << '"' << (*p)[static_cast<size_t>(idx)] << '"';
    else
      os << "<?>";
    return;
  }
  if (auto* p = lits.as<std::vector<value::token>>()) {
    if (idx >= 0 && static_cast<size_t>(idx) < p->size())
      os << '"' << (*p)[static_cast<size_t>(idx)].str() << '"';
    else
      os << "<?>";
    return;
  }
  os << "<?>";
}

// Simple brute-force way..
// TODO: Use std::function or some template technique?
// NOTE: Use dedicated path for `float` and `double`

#define CASE_EXPR_LIST(__FUNC) \
  __FUNC(bool)                 \
  __FUNC(half)                 \
  __FUNC(half2)                \
  __FUNC(half3)                \
  __FUNC(half4)                \
  __FUNC(int16_t)              \
  __FUNC(uint16_t)             \
  __FUNC(int32_t)              \
  __FUNC(uint32_t)             \
  __FUNC(char2)                \
  __FUNC(char3)                \
  __FUNC(char4)                \
  __FUNC(uchar2)               \
  __FUNC(uchar3)               \
  __FUNC(uchar4)               \
  __FUNC(short2)               \
  __FUNC(short3)               \
  __FUNC(short4)               \
  __FUNC(ushort2)              \
  __FUNC(ushort3)              \
  __FUNC(ushort4)              \
  __FUNC(int2)                 \
  __FUNC(int3)                 \
  __FUNC(int4)                 \
  __FUNC(uint2)                \
  __FUNC(uint3)                \
  __FUNC(uint4)                \
  __FUNC(int64_t)              \
  __FUNC(uint64_t)             \
  __FUNC(float2)               \
  __FUNC(float3)               \
  __FUNC(float4)               \
  __FUNC(double2)              \
  __FUNC(double3)              \
  __FUNC(double4)              \
  __FUNC(matrix2f)             \
  __FUNC(matrix3f)             \
  __FUNC(matrix4f)             \
  __FUNC(matrix2d)             \
  __FUNC(matrix3d)             \
  __FUNC(matrix4d)             \
  __FUNC(quath)                \
  __FUNC(quatf)                \
  __FUNC(quatd)                \
  __FUNC(normal3h)             \
  __FUNC(normal3f)             \
  __FUNC(normal3d)             \
  __FUNC(vector3h)             \
  __FUNC(vector3f)             \
  __FUNC(vector3d)             \
  __FUNC(point3h)              \
  __FUNC(point3f)              \
  __FUNC(point3d)              \
  __FUNC(color3h)              \
  __FUNC(color3f)              \
  __FUNC(color3d)              \
  __FUNC(color4h)              \
  __FUNC(color4f)              \
  __FUNC(color4d)              \
  __FUNC(texcoord2h)           \
  __FUNC(texcoord2f)           \
  __FUNC(texcoord2d)           \
  __FUNC(texcoord3h)           \
  __FUNC(texcoord3f)           \
  __FUNC(texcoord3d)           \
  __FUNC(frame4d)              \
  __FUNC(timecode)



std::string pprint_value(const value::Value &v, const uint32_t indent,
                         bool closing_brace) {
  // Schema prim types (Model/Xform/GeomMesh/.../SpatialAudio) are rendered
  // out-of-line by pprint_prim_value() in value-pprint-prim.cc — same
  // [MODEL_BEGIN, MODEL_END) detection Prim uses — so this TU need not
  // instantiate v.as<PrimType>()/to_string() for ~60 schema types.
  {
    const uint32_t prim_tid = v.type_id();
    if (prim_tid >= static_cast<uint32_t>(value::TypeId::TYPE_ID_MODEL_BEGIN) &&
        prim_tid < static_cast<uint32_t>(value::TypeId::TYPE_ID_MODEL_END)) {
      return pprint_prim_value(v, indent, closing_brace);
    }
  }
#define BASETYPE_CASE_EXPR(__ty)                           \
  case TypeTraits<__ty>::type_id(): {                      \
    auto p = v.as<__ty>();                                 \
    if (p) {                                               \
      os << (*p);                                          \
    } else {                                               \
      os << "[InternalError: Base type TypeId mismatch.]"; \
    }                                                      \
    break;                                                 \
  }


#define ARRAY1DTYPE_CASE_EXPR(__ty)                      \
  case TypeTraits<std::vector<__ty>>::type_id(): {       \
    if (auto p = v.as<std::vector<__ty>>()) {            \
      os << (*p);                                        \
    } else if (auto tp = v.as<TypedArray<__ty>>()) {            \
      os << (*tp);                                        \
    } else if (auto ctp = v.as<ChunkedTypedArray<__ty>>()) {            \
      os << (*ctp);                                        \
    } else {                                             \
      os << "[InternalError: 1D type TypeId mismatch.]"; \
    }                                                    \
    break;                                               \
  }

  std::stringstream os;

  switch (v.type_id()) {
    // base type
    CASE_EXPR_LIST(BASETYPE_CASE_EXPR)

    case TypeTraits<float>::type_id(): {
      auto p = v.as<float>();
      if (p) {
        os << dtos(*p);
      } else {
        os << "[InternalError: TypeId mismatch(`float` expected).]";
      }
      break;
    }

    case TypeTraits<double>::type_id(): {
      auto p = v.as<double>();
      if (p) {
        os << dtos(*p);
      } else {
        os << "[InternalError: TypeId mismatch(`double` expected).]";
      }
      break;
    }

      // 1D array
      CASE_EXPR_LIST(ARRAY1DTYPE_CASE_EXPR)

    case TypeTraits<std::vector<float>>::type_id(): {
      auto p = v.as<std::vector<float>>();
      if (p) {
        os << (*p);
      } else {
        os << "[InternalError: TypeId mismatch(`float[]` expected).]";
      }
      break;
    }

    case TypeTraits<std::vector<double>>::type_id(): {
      auto p = v.as<std::vector<double>>();
      if (p) {
        os << (*p);
      } else {
        os << "[InternalError: TypeId mismatch(`double[]` expected).]";
      }
      break;
    }

      // 2D array
      // CASE_EXPR_LIST(ARRAY2DTYPE_CASE_EXPR)


    // dict and customData
    case TypeTraits<CustomDataType>::type_id(): {
      auto p = v.as<CustomDataType>();
      if (p) {
        os << print_customData(*p, "", indent);
      } else {
        os << "[InternalError: Dict type TypeId mismatch.]";
      }
      break;
    }
    case TypeTraits<value::AssetPath>::type_id(): {
      auto p = v.as<value::AssetPath>();
      if (p) {
        os << (*p);
      } else {
        os << "[InternalError: asset type TypeId mismatch.]";
      }
      break;
    }
    case TypeTraits<std::vector<value::AssetPath>>::type_id(): {
      auto p = v.as<std::vector<value::AssetPath>>();
      if (p) {
        os << (*p);
      } else {
        os << "[InternalError: asset[] type TypeId mismatch.]";
      }
      break;
    }

    case TypeTraits<value::PathExpression>::type_id(): {
      auto p = v.as<value::PathExpression>();
      if (p) {
        os << (*p);
      } else {
        os << "[InternalError: pathExpression type TypeId mismatch.]";
      }
      break;
    }
    case TypeTraits<std::vector<value::PathExpression>>::type_id(): {
      auto p = v.as<std::vector<value::PathExpression>>();
      if (p) {
        os << (*p);
      } else {
        os << "[InternalError: pathExpression[] type TypeId mismatch.]";
      }
      break;
    }

    case TypeTraits<value::token>::type_id(): {
      auto p = v.as<value::token>();
      if (p) {
        os << buildEscapedAndQuotedStringForUSDA(p->str());
      } else {
        os << "[InternalError: Token type TypeId mismatch.]";
      }
      break;
    }
    case TypeTraits<std::vector<value::token>>::type_id(): {
      auto p = v.get_value<std::vector<value::token>>();
      if (p) {
        std::vector<std::string> vs;
        std::transform(p->begin(), p->end(), std::back_inserter(vs),
                       [](const value::token &tok) {
                         return buildEscapedAndQuotedStringForUSDA(tok.str());
                       });

        os << vs;
      } else {
        os << "[InternalError: `token[]` type TypeId mismatch.]";
      }
      break;
    }
    case TypeTraits<std::string>::type_id(): {
      auto p = v.as<std::string>();
      if (p) {
        os << buildEscapedAndQuotedStringForUSDA(*p);
      } else {
        os << "[InternalError: `string` type TypeId mismatch.]";
      }
      break;
    }
    case TypeTraits<value::StringData>::type_id(): {
      auto p = v.as<value::StringData>();
      if (p) {
        os << buildEscapedAndQuotedStringForUSDA(p->value);
      } else {
        os << "[InternalError: `string` type TypeId mismatch.]";
      }
      break;
    }
    case TypeTraits<std::vector<std::string>>::type_id(): {
      auto p = v.as<std::vector<std::string>>();
      if (p) {
        std::vector<std::string> ss;
        for (const auto &item : *p) {
          ss.push_back(buildEscapedAndQuotedStringForUSDA(item));
        }
        os << ss;  // Use operator<<(std::vector<std::string>)
      } else {
        os << "[InternalError: `string[]` type TypeId mismatch.]";
      }
      break;
    }
    case TypeTraits<std::vector<value::StringData>>::type_id(): {
      auto p = v.as<std::vector<value::StringData>>();
      if (p) {
        os << (*p);
      } else {
        os << "[InternalError: `string[]` type TypeId mismatch.]";
      }
      break;
    }
    // uchar (uint8_t) needs special handling: os << uint8_t prints as char
    case TypeTraits<char>::type_id(): {
      auto p = v.as<char>();
      if (p) {
        os << static_cast<int>(*p);
      } else {
        os << "[InternalError: char TypeId mismatch.]";
      }
      break;
    }
    case TypeTraits<std::vector<char>>::type_id(): {
      if (auto p = v.as<std::vector<char>>()) {
        os << "[";
        for (size_t i = 0; i < p->size(); ++i) {
          if (i) {
            os << ", ";
          }
          os << static_cast<int>((*p)[i]);
        }
        os << "]";
      } else {
        os << "[InternalError: char[] TypeId mismatch.]";
      }
      break;
    }
    case TypeTraits<uint8_t>::type_id(): {
      auto p = v.as<uint8_t>();
      if (p) {
        os << static_cast<int>(*p);
      } else {
        os << "[InternalError: uchar TypeId mismatch.]";
      }
      break;
    }
    // uchar[] (std::vector<uint8_t>): operator<< prints elements as integers.
    case TypeTraits<std::vector<uint8_t>>::type_id(): {
      if (auto p = v.as<std::vector<uint8_t>>()) {
        os << (*p);
      } else {
        os << "[InternalError: uchar[] TypeId mismatch.]";
      }
      break;
    }
    case TypeTraits<value::ValueBlock>::type_id(): {
      if (v.as<value::ValueBlock>()) {
        os << "None";
      } else {
        os << "[InternalError: ValueBlock type TypeId mismatch.]";
      }
      break;
    }
    case TypeTraits<value::AnimationBlock>::type_id(): {
      if (v.as<value::AnimationBlock>()) {
        os << "AnimationBlock";
      } else {
        os << "[InternalError: AnimationBlock type TypeId mismatch.]";
      }
      break;
    }
    case TypeTraits<value::ArrayEdit>::type_id(): {
      const value::ArrayEdit* ae = v.as<value::ArrayEdit>();
      if (!ae) {
        os << "[InternalError: ArrayEdit type TypeId mismatch.]";
        break;
      }
      // OpenUSD `edit [<op>; <op>; ...]` form (see Vt_ArrayEditStreamImpl).
      os << "edit [";
      const std::vector<int64_t>& ins = ae->ops;
      auto idxstr = [&](int64_t x) { os << '[' << x << ']'; };
      size_t i = 0;
      size_t emitted = 0;
      while (i < ins.size()) {
        const value::ArrayEditOp op = value::ArrayEditWordOp(ins[i]);
        const int64_t count = value::ArrayEditWordCount(ins[i]);
        ++i;
        const int arity = value::ArrayEditOpArity(op);
        for (int64_t c = 0; c < count; c++) {
          if (i + static_cast<size_t>(arity) > ins.size()) {
            i = ins.size();
            break;
          }
          const int64_t a1 = ins[i++];
          const int64_t a2 = (arity == 2) ? ins[i++] : 0;
          if (emitted++) os << "; ";
          switch (op) {
            case value::ArrayEditOp::WriteLiteral:
              os << "write "; PrintArrayEditLiteral(os, ae->literals, a1);
              os << " to "; idxstr(a2);
              break;
            case value::ArrayEditOp::WriteRef:
              os << "write "; idxstr(a1); os << " to "; idxstr(a2);
              break;
            case value::ArrayEditOp::InsertLiteral:
              if (a2 == 0) {
                os << "prepend "; PrintArrayEditLiteral(os, ae->literals, a1);
              } else if (a2 == value::kArrayEditEndIndex) {
                os << "append "; PrintArrayEditLiteral(os, ae->literals, a1);
              } else {
                os << "insert "; PrintArrayEditLiteral(os, ae->literals, a1);
                os << " at "; idxstr(a2);
              }
              break;
            case value::ArrayEditOp::InsertRef:
              if (a2 == 0) {
                os << "prepend "; idxstr(a1);
              } else if (a2 == value::kArrayEditEndIndex) {
                os << "append "; idxstr(a1);
              } else {
                os << "insert "; idxstr(a1); os << " at "; idxstr(a2);
              }
              break;
            case value::ArrayEditOp::EraseRef:
              os << "erase "; idxstr(a1);
              break;
            case value::ArrayEditOp::MinSize:
              os << "minsize " << a1;
              break;
            case value::ArrayEditOp::MinSizeFill:
              os << "minsize " << a1 << " fill ";
              PrintArrayEditLiteral(os, ae->literals, a2);
              break;
            case value::ArrayEditOp::SetSize:
              os << "resize " << a1;
              break;
            case value::ArrayEditOp::SetSizeFill:
              os << "resize " << a1 << " fill ";
              PrintArrayEditLiteral(os, ae->literals, a2);
              break;
            case value::ArrayEditOp::MaxSize:
              os << "maxsize " << a1;
              break;
          }
        }
      }
      os << "]";
      break;
    }
    // TODO: List-up all case and remove `default` clause.
    default: {
      os << "VALUE_PPRINT: TODO: (type: " << v.type_name() << ") ";
    }
  }

#undef BASETYPE_CASE_EXPR
#undef ARRAY1DTYPE_CASE_EXPR
#undef ARRAY2DTYPE_CASE_EXPR

  return os.str();
}

#undef CASE_EXPR_LIST


}  // namespace value
}  // namespace lightusd

// SPDX-License-Identifier: Apache-2.0
#include "gui_stringify.hh"

#include "pprint-enum.hh"
#include "usdGeom.hh"
#include "value-pprint.hh"

#include <map>

namespace lusdview {

namespace {
std::string Sanitize(std::string s, size_t maxLen = 240) {
  for (char& c : s) {
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  }
  if (s.size() > maxLen) {
    s.resize(maxLen);
    s += " ...";
  }
  return s;
}
}  // namespace

std::string PropertyToString(const lightusd::legacy::Property& prop) {
  if (prop.is_relationship()) {
    std::vector<lightusd::legacy::Path> targets = prop.get_relationTargets();
    if (targets.empty()) return "(no targets)";
    std::string s;
    for (size_t i = 0; i < targets.size(); ++i) {
      if (i) s += ", ";
      s += targets[i].full_path_name();
    }
    return Sanitize(s);
  }

  if (prop.is_attribute()) {
    const lightusd::legacy::Attribute& a = prop.get_attribute();
    if (a.has_connections()) {
      const auto& conns = a.connections();
      std::string s;
      for (size_t i = 0; i < conns.size(); ++i) {
        if (i) s += ", ";
        s += conns[i].full_path_name();
      }
      return Sanitize("<connect: " + s + ">");
    }
    if (a.has_value()) {
      const lightusd::legacy::value::Value& v = a.get_var().value_raw();
      return Sanitize(lightusd::legacy::value::pprint_value(v, 0, /*closing_brace=*/false));
    }
    if (a.has_timesamples()) {
      return "(timeSamples)";
    }
    std::string t = prop.value_type_name();
    return t.empty() ? "(no value)" : ("(" + t + ", no value)");
  }

  return "(empty)";
}

std::string PrimMetaSummary(const lightusd::legacy::Prim& prim) {
  const auto& m = prim.metas();
  std::string s;
  auto add = [&](const char* k, const std::string& v) {
    if (!s.empty()) s += "\n";
    s += k;
    s += ": ";
    s += v;
  };
  if (m.has_kind()) add("kind", m.get_kind_str());
  if (m.has_active()) add("active", m.get_active() ? "true" : "false");
  if (m.has_hidden()) add("hidden", m.get_hidden() ? "true" : "false");
  if (m.has_displayName()) add("displayName", m.get_displayName());
  if (m.has_doc()) add("documentation", m.get_doc().value);
  if (m.has_comment()) add("comment", m.get_comment().value);
  if (m.has_customData()) add("customData", "(authored)");

  // apiSchemas
  if (m.has_apiSchemas()) {
    auto schemas = m.get_apiSchemas();
    if (!schemas.names.empty() || !schemas.unknownSchemas.empty()) {
      std::string val;
      for (const auto& ns : schemas.names) {
        if (!val.empty()) val += ", ";
        val += lightusd::legacy::to_string(ns.first);
        if (!ns.second.empty()) val += ":" + ns.second;
      }
      for (const auto& us : schemas.unknownSchemas) {
        if (!val.empty()) val += ", ";
        val += us.first;
        if (!us.second.empty()) val += ":" + us.second;
      }
      add("apiSchemas", val);
    }
  }

  // Variant selections
  if (m.variants.has_value() && !m.variants->empty()) {
    std::string val;
    for (const auto& kv : *m.variants) {
      if (!val.empty()) val += ", ";
      val += kv.first + "=" + kv.second;
    }
    add("variants", val);
  }

  // Composition arcs
  if (m.references.has_value() && !m.references->empty()) {
    add("references", "(" + std::to_string(m.references->size()) + " ref(s))");
  }
  if (m.payload.has_value()) {
    add("payload", "(authored)");
  }
  if (m.inherits.has_value() && !m.inherits->empty()) {
    add("inherits", "(" + std::to_string(m.inherits->size()) + " path(s))");
  }
  if (m.specializes.has_value() && !m.specializes->empty()) {
    add("specializes", "(" + std::to_string(m.specializes->size()) + " path(s))");
  }

  return s;
}

std::string AttrMetaSummary(const lightusd::legacy::Attribute& attr) {
  const auto& m = attr.metas();
  std::string s;
  auto add = [&](const std::string& kv) {
    if (!s.empty()) s += "  ";
    s += kv;
  };
  if (m.has_interpolation()) add("interpolation=" + m.get_interpolation().str());
  if (m.has_elementSize()) add("elementSize=" + std::to_string(m.get_elementSize()));
  if (m.has_displayName()) add("displayName=" + m.get_displayName());
  if (m.has_customData()) add("customData");
  return s;
}

std::string GPrimPropertySummary(const lightusd::legacy::Prim& prim) {
  std::string s;
  auto add = [&](const char* k, const std::string& v) {
    if (!s.empty()) s += "\n";
    s += k;
    s += ": ";
    s += v;
  };

  auto tryGPrim = [&](auto* gprim) -> bool {
    if (gprim->doubleSided.authored()) {
      add("doubleSided", gprim->doubleSided.get_value() ? "true" : "false");
    }
    add("orientation", lightusd::legacy::to_string(gprim->orientation.get_value()));
    if (gprim->visibility.authored()) {
      lightusd::legacy::Visibility vis;
      if (gprim->visibility.get_value().get_default(&vis)) {
        add("visibility", lightusd::legacy::to_string(vis));
      }
    }
    if (gprim->purpose.authored()) {
      add("purpose", lightusd::legacy::to_string(gprim->purpose.get_value()));
    }
    return true;
  };

  if (auto* m = prim.as<lightusd::legacy::GeomMesh>()) tryGPrim(m);
  else if (auto* gs = prim.as<lightusd::legacy::GeomSphere>()) tryGPrim(gs);
  else if (auto* c = prim.as<lightusd::legacy::GeomCube>()) tryGPrim(c);
  else if (auto* y = prim.as<lightusd::legacy::GeomCylinder>()) tryGPrim(y);
  else if (auto* o = prim.as<lightusd::legacy::GeomCone>()) tryGPrim(o);
  else if (auto* p = prim.as<lightusd::legacy::GeomCapsule>()) tryGPrim(p);
  else if (auto* n = prim.as<lightusd::legacy::GeomPlane>()) tryGPrim(n);
  else if (auto* b = prim.as<lightusd::legacy::GeomBasisCurves>()) tryGPrim(b);
  else if (auto* pt = prim.as<lightusd::legacy::GeomPoints>()) tryGPrim(pt);

  return s;
}

std::string SubdivisionSchemeName(const lightusd::legacy::Prim& prim) {
  if (const auto* m = prim.as<lightusd::legacy::GeomMesh>()) {
    if (m->subdivisionScheme.authored()) {
      auto v = m->subdivisionScheme.get_value();
      switch (v) {
        case lightusd::legacy::GeomMesh::SubdivisionScheme::CatmullClark: return "catmullClark";
        case lightusd::legacy::GeomMesh::SubdivisionScheme::Loop: return "loop";
        case lightusd::legacy::GeomMesh::SubdivisionScheme::Bilinear: return "bilinear";
        case lightusd::legacy::GeomMesh::SubdivisionScheme::SubdivisionSchemeNone: return "none";
        default: return "none";
      }
    }
  }
  return {};
}

std::string VisibilityState(const lightusd::legacy::Prim& prim) {
  auto tryVis = [&](auto* gprim) -> std::string {
    if (gprim->visibility.authored()) {
      lightusd::legacy::Visibility vis;
      if (gprim->visibility.get_value().get_default(&vis)) {
        return lightusd::legacy::to_string(vis);
      }
    }
    return {};
  };

  if (auto* m = prim.as<lightusd::legacy::GeomMesh>()) return tryVis(m);
  else if (auto* s = prim.as<lightusd::legacy::GeomSphere>()) return tryVis(s);
  else if (auto* c = prim.as<lightusd::legacy::GeomCube>()) return tryVis(c);
  else if (auto* y = prim.as<lightusd::legacy::GeomCylinder>()) return tryVis(y);
  else if (auto* o = prim.as<lightusd::legacy::GeomCone>()) return tryVis(o);
  else if (auto* p = prim.as<lightusd::legacy::GeomCapsule>()) return tryVis(p);
  else if (auto* n = prim.as<lightusd::legacy::GeomPlane>()) return tryVis(n);
  else if (auto* b = prim.as<lightusd::legacy::GeomBasisCurves>()) return tryVis(b);
  else if (auto* pt = prim.as<lightusd::legacy::GeomPoints>()) return tryVis(pt);

  return {};
}

std::string VariantSetDetail(const lightusd::legacy::Prim& prim) {
  const auto& vs = prim.variantSets();
  if (vs.empty()) return {};

  std::map<std::string, std::string> selections;
  const auto& metas = prim.metas();
  if (metas.variants.has_value()) selections = *metas.variants;

  std::string s;
  for (const auto& kv : vs) {
    const std::string& setName = kv.first;
    const auto& vspec = kv.second;
    if (!s.empty()) s += "\n";
    s += setName + " = {";
    bool first = true;
    for (const auto& opt : vspec.variantSet) {
      if (!first) s += ", ";
      s += opt.first;
      first = false;
    }
    s += "}";
    auto it = selections.find(setName);
    if (it != selections.end()) s += "  (selected: " + it->second + ")";
  }
  return s;
}

std::string GeometrySummary(const lightusd::legacy::Prim& prim) {
  std::string s;

  if (const auto* m = prim.as<lightusd::legacy::GeomMesh>()) {
    size_t nVerts = 0, nFaces = 0, nTris = 0;
    if (m->points.authored()) {
      auto pts = m->get_points();
      nVerts = pts.size();
    }
    if (m->faceVertexCounts.authored()) {
      auto counts = m->get_faceVertexCounts();
      nFaces = counts.size();
      for (int c : counts) {
        if (c == 3) nTris++;
        else if (c > 3) nTris += c - 2;
      }
    }
    s = std::to_string(nVerts) + " verts, " + std::to_string(nFaces) + " faces";
    if (nTris > 0 && nTris != nFaces) s += " (~" + std::to_string(nTris) + " tris)";
  } else if (const auto* sp = prim.as<lightusd::legacy::GeomSphere>()) {
    double r = 1.0;
    sp->radius.get_value().get_default(&r);
    s = "radius: " + std::to_string(r);
  } else if (const auto* cu = prim.as<lightusd::legacy::GeomCube>()) {
    double sz = 2.0;
    cu->size.get_value().get_default(&sz);
    s = "size: " + std::to_string(sz);
  } else if (const auto* cy = prim.as<lightusd::legacy::GeomCylinder>()) {
    double r = 1.0, h = 2.0;
    cy->radius.get_value().get_default(&r);
    cy->height.get_value().get_default(&h);
    s = "radius: " + std::to_string(r) + ", height: " + std::to_string(h);
  } else if (const auto* co = prim.as<lightusd::legacy::GeomCone>()) {
    double r = 1.0, h = 2.0;
    co->radius.get_value().get_default(&r);
    co->height.get_value().get_default(&h);
    s = "radius: " + std::to_string(r) + ", height: " + std::to_string(h);
  } else if (const auto* ca = prim.as<lightusd::legacy::GeomCapsule>()) {
    double r = 0.5, h = 1.0;
    ca->radius.get_value().get_default(&r);
    ca->height.get_value().get_default(&h);
    s = "radius: " + std::to_string(r) + ", height: " + std::to_string(h);
  } else if (const auto* bc = prim.as<lightusd::legacy::GeomBasisCurves>()) {
    size_t nPts = 0;
    if (bc->points.authored()) {
      auto pts = bc->get_points();
      nPts = pts.size();
    }
    s = std::to_string(nPts) + " points";
  } else if (const auto* pt = prim.as<lightusd::legacy::GeomPoints>()) {
    size_t nPts = 0;
    if (pt->points.authored()) {
      auto pts = pt->get_points();
      nPts = pts.size();
    }
    s = std::to_string(nPts) + " points";
  }

  return s;
}

}  // namespace lusdview

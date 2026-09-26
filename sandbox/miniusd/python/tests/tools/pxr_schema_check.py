"""Schema conformance check with pxr python (MINIUSD_PXR_PYTHON): every authored
attribute that a prim/API schema defines must have the schema type and
variability, and every authored API schema must be recognized.

Prints 'ISSUE <text>' lines and a final 'CHECKED <n>' line."""
import sys

from pxr import Usd

for f in sys.argv[1:]:
    st = Usd.Stage.Open(f)
    n = 0
    for p in st.Traverse():
        if p.GetTypeName() and not p.GetPrimTypeInfo().GetSchemaType():
            print("ISSUE %s: unknown prim type %s" % (p.GetPath(), p.GetTypeName()))
        lo = p.GetMetadata("apiSchemas")
        applied = set(p.GetAppliedSchemas())
        for name in (lo.GetAddedOrExplicitItems() if lo else []):
            if name not in applied:
                print("ISSUE %s: API schema %s not recognized" % (p.GetPath(), name))
        d = p.GetPrimDefinition()
        for a in p.GetAuthoredAttributes():
            spec = d.GetSchemaAttributeSpec(a.GetName())
            if spec is None:
                continue
            n += 1
            if spec.typeName != a.GetTypeName():
                print("ISSUE %s: type %s != schema %s" % (a.GetPath(), a.GetTypeName(), spec.typeName))
            if spec.variability != a.GetVariability():
                print("ISSUE %s: variability %s != schema %s" % (a.GetPath(), a.GetVariability(),
                                                                spec.variability))
        n += sum(1 for r in p.GetAuthoredRelationships() if d.GetSchemaRelationshipSpec(r.GetName()))
    print("CHECKED %d" % n)

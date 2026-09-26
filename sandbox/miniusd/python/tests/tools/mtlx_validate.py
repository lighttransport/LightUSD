"""Validate .mtlx files with the MaterialX python package (MINIUSD_MTLX_PYTHON)."""
import sys, MaterialX as mx
lib = mx.createDocument(); mx.loadLibraries(mx.getDefaultDataLibraryFolders(), mx.getDefaultDataSearchPath(), lib)
for f in sys.argv[1:]:
    doc = mx.createDocument(); mx.readFromXmlFile(doc, f); doc.importLibrary(lib)
    ok, msg = doc.validate()
    missing = [n.getNamePath() for n in doc.traverseTree() if isinstance(n, mx.Node) and not n.getNodeDef()]
    print(f.split('/')[-1], 'valid' if ok else 'INVALID', msg.strip()[:300], 'missing-nodedefs:', missing)

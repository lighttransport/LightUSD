// MaterialX XML (C11 core, src/c/mtlx/mtlx-xml.c): both parsers, the
// resolved-XML writer and the XMLNode normalisation on arbitrary input.
#include <cstdint>
#include <cstring>

#include "c/mtlx/mtlx-internal.h"

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const *data, std::size_t size) {
  if (size > 1024 * 1024 * 16) return 0;  // 16 MB cap
  const char *xml = reinterpret_cast<const char *>(data);
  for (int checked = 0; checked < 2; checked++) {
    lu_mtlx_elem root;
    lu_mtlx_elem_init(&root);
    lu_mtlx_sb err, warn, out;
    lu_mtlx_sb_init(&err);
    lu_mtlx_sb_init(&warn);
    lu_mtlx_sb_init(&out);
    const int ok = checked ? lu_mtlx_parse_xml_checked(xml, size, &root, &err, &warn)
                           : lu_mtlx_parse_xml(xml, size, &root, &err);
    if (ok) {
      lu_mtlx_write_xml(&root, &out);
      lu_mtlx_xmlnode_normalize(&root);
    }
    lu_mtlx_elem_free(&root);
    lu_mtlx_sb_free(&err);
    lu_mtlx_sb_free(&warn);
    lu_mtlx_sb_free(&out);
  }
  return 0;
}

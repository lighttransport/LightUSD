/* Native MaterialX VDF graph and phase-lobe regression. */
#include "mtlx_doc.h"
#include "mtlx_eval.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int evaluate(const char *xml, MtlxVolumeParams *out) {
    MtlxDoc *doc = mtlx_load_string(xml);
    if (!doc) return 0;
    /* Model an in-memory graph whose nodedef defaults have not been inserted. */
    for (int i = 0; i < doc->nnode; ++i) {
        MtlxNode *n = &doc->nodes[i];
        if (strcmp(n->name, "defaultmix") && strcmp(n->name, "vm") && strcmp(n->name, "em")) continue;
        for (int j = 0; j < n->ninput; ++j)
            if (!strcmp(n->inputs[j].name, "mix")) n->inputs[j].name[0] = '_';
    }
    int id = mtlx_find_node(doc, -1, "V");
    ShadeContext ctx; memset(&ctx, 0, sizeof(ctx)); ctx.doc = doc;
    ctx.memo = calloc((size_t)doc->nnode, sizeof(*ctx.memo));
    ctx.memo_done = calloc((size_t)doc->nnode, 1);
    int ok = ctx.memo && ctx.memo_done && !mtlx_eval_volume(&ctx, id, out);
    free(ctx.memo); free(ctx.memo_done); mtlx_free(doc); return ok;
}
static int near(float a, float b) { return fabsf(a - b) < 1e-5f; }
int main(void) {
    const char *weighted =
        "<materialx version=\"1.39\">"
        "<anisotropic_vdf name=\"a\" type=\"VDF\"><input name=\"scattering\" type=\"vector3\" value=\"1,2,3\"/><input name=\"anisotropy\" type=\"float\" value=\"0.6\"/></anisotropic_vdf>"
        "<anisotropic_vdf name=\"b\" type=\"VDF\"><input name=\"scattering\" type=\"vector3\" value=\"4,5,6\"/><input name=\"anisotropy\" type=\"float\" value=\"-0.6\"/></anisotropic_vdf>"
        "<multiply name=\"w\" type=\"VDF\"><input name=\"in1\" type=\"VDF\" nodename=\"b\"/><input name=\"in2\" type=\"color3\" value=\"0.25,0.5,0.75\"/></multiply>"
        "<add name=\"sum\" type=\"VDF\"><input name=\"in1\" type=\"VDF\" nodename=\"a\"/><input name=\"in2\" type=\"VDF\" nodename=\"w\"/></add>"
        "<uniform_edf name=\"e\" type=\"EDF\"><input name=\"color\" type=\"color3\" value=\"0.1,0.2,0.3\"/></uniform_edf>"
        "<multiply name=\"ew\" type=\"EDF\"><input name=\"in1\" type=\"EDF\" nodename=\"e\"/><input name=\"in2\" type=\"color3\" value=\"0.25,0.5,0.75\"/></multiply>"
        "<volume name=\"V\" type=\"volumeshader\"><input name=\"vdf\" type=\"VDF\" nodename=\"sum\"/><input name=\"edf\" type=\"EDF\" nodename=\"ew\"/></volume></materialx>";
    MtlxVolumeParams out;
    if (!evaluate(weighted, &out) || out.lobe_count != 2 ||
        !near(out.lobes[0].anisotropy, .6f) || !near(out.lobes[1].anisotropy, -.6f) ||
        !near(out.lobes[1].scattering.x, 1) || !near(out.lobes[1].scattering.y, 2.5f) ||
        !near(out.lobes[1].scattering.z, 4.5f) || !near(out.scattering.z, 7.5f) ||
        !near(out.emission.x, .025f) || !near(out.emission.y, .1f) ||
        !near(out.emission.z, .225f)) { fprintf(stderr, "weighted VDF failed\n"); return 1; }
    /* Missing mix defaults to 0 for both VDF and EDF (pbrlib nodedefs). */
    const char *default_mix =
        "<materialx><anisotropic_vdf name=\"bg\" type=\"VDF\"><input name=\"scattering\" type=\"vector3\" value=\"1,2,3\"/></anisotropic_vdf>"
        "<anisotropic_vdf name=\"fg\" type=\"VDF\"><input name=\"scattering\" type=\"vector3\" value=\"4,5,6\"/></anisotropic_vdf>"
        "<mix name=\"vm\" type=\"VDF\"><input name=\"bg\" type=\"VDF\" nodename=\"bg\"/><input name=\"fg\" type=\"VDF\" nodename=\"fg\"/></mix>"
        "<uniform_edf name=\"eb\" type=\"EDF\"><input name=\"color\" type=\"color3\" value=\"0.1,0.2,0.3\"/></uniform_edf>"
        "<uniform_edf name=\"ef\" type=\"EDF\"><input name=\"color\" type=\"color3\" value=\"0.6,0.7,0.8\"/></uniform_edf>"
        "<mix name=\"em\" type=\"EDF\"><input name=\"bg\" type=\"EDF\" nodename=\"eb\"/><input name=\"fg\" type=\"EDF\" nodename=\"ef\"/></mix>"
        "<volume name=\"V\" type=\"volumeshader\"><input name=\"vdf\" type=\"VDF\" nodename=\"vm\"/><input name=\"edf\" type=\"EDF\" nodename=\"em\"/></volume></materialx>";
    if (!evaluate(default_mix, &out) || !near(out.scattering.x, 1.0f) ||
        !near(out.scattering.y, 2.0f) || !near(out.scattering.z, 3.0f) ||
        !near(out.emission.x, .1f) || !near(out.emission.y, .2f) || !near(out.emission.z, .3f)) {
        fprintf(stderr, "unauthored VDF/EDF mix failed\n"); return 1;
    }
    const char *bad[] = {
        "<materialx><volume name=\"V\" type=\"volumeshader\"><input name=\"vdf\" type=\"VDF\" nodename=\"missing\"/></volume></materialx>",
        "<materialx><unsupported name=\"bad\" type=\"VDF\"/><volume name=\"V\" type=\"volumeshader\"><input name=\"vdf\" type=\"VDF\" nodename=\"bad\"/></volume></materialx>",
        "<materialx><add name=\"loop\" type=\"VDF\"><input name=\"in1\" type=\"VDF\" nodename=\"loop\"/></add><volume name=\"V\" type=\"volumeshader\"><input name=\"vdf\" type=\"VDF\" nodename=\"loop\"/></volume></materialx>",
        "<materialx><measured_edf name=\"e\" type=\"EDF\"/><volume name=\"V\" type=\"volumeshader\"><input name=\"edf\" type=\"EDF\" nodename=\"e\"/></volume></materialx>"
    };
    for (size_t i = 0; i < sizeof(bad)/sizeof(*bad); ++i) if (evaluate(bad[i], &out)) {
        fprintf(stderr, "invalid volume accepted: %zu\n", i); return 1;
    }
    char *xml = calloc(1, 65536); if (!xml) return 1;
    size_t len = (size_t)snprintf(xml, 65536, "<materialx>");
    int ids[65], n = 65, next = 0;
    for (int i = 0; i < n; ++i) {
        ids[i] = i;
        len += (size_t)snprintf(xml + len, 65536 - len,
          "<anisotropic_vdf name=\"n%d\" type=\"VDF\"><input name=\"scattering\" type=\"vector3\" value=\"1,0,0\"/><input name=\"anisotropy\" type=\"float\" value=\"%f\"/></anisotropic_vdf>", i, i / 100.0);
    }
    next = n;
    while (n > 1) {
        int count = 0;
        for (int i = 0; i < n; i += 2) {
            if (i + 1 == n) { ids[count++] = ids[i]; continue; }
            int id = next++;
            len += (size_t)snprintf(xml + len, 65536 - len,
                "<add name=\"n%d\" type=\"VDF\"><input name=\"in1\" type=\"VDF\" nodename=\"n%d\"/><input name=\"in2\" type=\"VDF\" nodename=\"n%d\"/></add>", id, ids[i], ids[i + 1]);
            ids[count++] = id;
        }
        n = count;
    }
    snprintf(xml + len, 65536 - len, "<volume name=\"V\" type=\"volumeshader\"><input name=\"vdf\" type=\"VDF\" nodename=\"n%d\"/></volume></materialx>", ids[0]);
    char *last_g = strstr(xml, "0.640000");
    if (!last_g) { free(xml); return 1; }
    memcpy(last_g, "0.000000", 8);
    if (!evaluate(xml, &out) || out.lobe_count != 64 ||
        !near(out.lobes[0].scattering.x, 2.0f)) {
        free(xml); fprintf(stderr, "equal phase lobes did not combine\n"); return 1;
    }
    memcpy(last_g, "0.640000", 8);
    int accepted = evaluate(xml, &out); free(xml);
    if (accepted) { fprintf(stderr, "lobe limit not enforced\n"); return 1; }
    puts("Native MaterialX volume lobes: PASS"); return 0;
}

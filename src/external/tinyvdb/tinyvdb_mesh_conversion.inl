/* Private implementation; included after the shared BVH helpers. */
static tvdb_status_t mesh_error(tvdb_error_t *err, tvdb_status_t status, const char *message) {
    if (err) {
        memset(err, 0, sizeof(*err));
        err->status = status;
        snprintf(err->message, sizeof(err->message), "%s", message);
    }
    return status;
}
/* Preflight arena destinations before its zero-initializing allocator can
 * touch borrowed input or an existing output. */
static void *mesh_arena_destination(const tvdb_arena_allocator_t *arena, size_t *offset,
                                    size_t bytes) {
    if (!arena->buffer || *offset > SIZE_MAX - 7)
        return NULL;
    size_t aligned = (*offset + 7) & ~(size_t)7;
    if (aligned > arena->buffer_size || bytes > arena->buffer_size - aligned ||
        aligned > UINTPTR_MAX - (uintptr_t)arena->buffer)
        return NULL;
    *offset = aligned + bytes;
    return (void *)((uintptr_t)arena->buffer + aligned);
}
static tvdb_status_t mesh_validate(const tvdb_triangle_mesh *m, tvdb_error_t *err) {
    if (!m || !m->vertices || !m->faces || !m->vertex_count || !m->face_count ||
        m->vertex_count > UINT32_MAX || m->face_count > INT32_MAX ||
        m->vertex_count > SIZE_MAX / sizeof(tvdb_vec3f) ||
        m->face_count > SIZE_MAX / (6 * sizeof(float)))
        return mesh_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid mesh arrays or counts");
    for (size_t i = 0; i < m->vertex_count; ++i) {
        tvdb_vec3f v = m->vertices[i];
        if (!isfinite(v.x) || !isfinite(v.y) || !isfinite(v.z))
            return mesh_error(err, TVDB_ERROR_INVALID_DATA, "nonfinite mesh vertex");
    }
    for (size_t i = 0; i < m->face_count; ++i) {
        tvdb_triangle f = m->faces[i];
        if (f.v0 >= m->vertex_count || f.v1 >= m->vertex_count || f.v2 >= m->vertex_count)
            return mesh_error(err, TVDB_ERROR_INVALID_DATA, "mesh face index out of range");
    }
    return TVDB_OK;
}
static tvdb_status_t mesh_pool(size_t work, size_t tasks, tvdb_thread_pool_t **pool,
                               tvdb_error_t *err) {
    size_t workers = work < 32768 ? 1 : tvdb_thread_default_count();
    if (workers > 8)
        workers = 8;
    if (workers > tasks)
        workers = tasks;
    if (!workers)
        workers = 1;
    return tvdb_thread_pool_create(workers, pool, err);
}
static int mesh_grid_frame(const tvdb_dense_grid *g, size_t *bytes) {
    if (!g || !tvdb_grid_valid(g->nx, g->ny, g->nz, g->voxel_size, g->data, sizeof(float)) ||
        !tvdb_grid_bytes(g->nx, g->ny, g->nz, sizeof(float), bytes))
        return 0;
    const float origin[3] = {g->ox, g->oy, g->oz};
    const int dim[3] = {g->nx, g->ny, g->nz};
    for (int a = 0; a < 3; ++a) {
        double lo = (double)origin[a] + 0.5 * g->voxel_size,
               hi = (double)origin[a] + (dim[a] - 0.5) * g->voxel_size;
        if (!isfinite(origin[a]) || !isfinite(lo) || !isfinite(hi) || fabs(lo) > FLT_MAX ||
            fabs(hi) > FLT_MAX)
            return 0;
    }
    return 1;
}
/* Preserve ordinary float lattice arithmetic; use double only if an
 * intermediate overflows despite a representable final world coordinate. */
static tvdb_vec3f mesh_position(const tvdb_dense_grid *g, int x, int y, int z) {
    tvdb_vec3f p = voxel_pos_c(g, x, y, z);
    if (!isfinite(p.x))
        p.x = (float)((double)g->ox + (x + 0.5) * g->voxel_size);
    if (!isfinite(p.y))
        p.y = (float)((double)g->oy + (y + 0.5) * g->voxel_size);
    if (!isfinite(p.z))
        p.z = (float)((double)g->oz + (z + 0.5) * g->voxel_size);
    return p;
}

struct tvdb_mesh_sdf {
    const tvdb_triangle_mesh *mesh;
    const tvdb_vec3f *vertices;
    const tvdb_triangle *faces;
    size_t vertex_count, face_count;
    tvdb_vec3f *normals;
    tvdb_bvh_t bvh;
    tvdb_vec3f low, high;
};
static tvdb_vec3f mesh_normal(tvdb_vec3f a, tvdb_vec3f b, tvdb_vec3f c) {
    tvdb_vec3f cross = cross_c(sub_c(b, a), sub_c(c, a));
    float length2 = dot_c(cross, cross);
    if (isfinite(length2) && length2 > 0)
        return normalize_c(cross);
    double ab[3] = {(double)b.x - a.x, (double)b.y - a.y, (double)b.z - a.z},
           ac[3] = {(double)c.x - a.x, (double)c.y - a.y, (double)c.z - a.z};
    double n[3] = {ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2],
                   ab[0] * ac[1] - ab[1] * ac[0]};
    double length = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!length)
        return (tvdb_vec3f){0, 0, 0};
    return (tvdb_vec3f){(float)(n[0] / length), (float)(n[1] / length), (float)(n[2] / length)};
}
tvdb_status_t tvdb_mesh_sdf_create(const tvdb_triangle_mesh *mesh, tvdb_mesh_sdf_t **out,
                                   tvdb_error_t *err) {
    if (!out)
        return mesh_error(err, TVDB_ERROR_INVALID_ARGUMENT, "null mesh workspace output");
    *out = NULL;
    tvdb_status_t st = mesh_validate(mesh, err);
    if (st != TVDB_OK)
        return st;
    tvdb_mesh_sdf_t *p = calloc(1, sizeof(*p));
    if (!p)
        return mesh_error(err, TVDB_ERROR_OUT_OF_MEMORY, "mesh workspace allocation failed");
    p->mesh = mesh;
    p->vertices = mesh->vertices;
    p->faces = mesh->faces;
    p->vertex_count = mesh->vertex_count;
    p->face_count = mesh->face_count;
    p->normals = malloc(mesh->face_count * sizeof(tvdb_vec3f));
    if (!p->normals)
        goto oom;
    p->low = p->high = mesh->vertices[0];
    for (size_t i = 1; i < mesh->vertex_count; ++i) {
        tvdb_vec3f v = mesh->vertices[i];
        p->low.x = fminf(p->low.x, v.x);
        p->low.y = fminf(p->low.y, v.y);
        p->low.z = fminf(p->low.z, v.z);
        p->high.x = fmaxf(p->high.x, v.x);
        p->high.y = fmaxf(p->high.y, v.y);
        p->high.z = fmaxf(p->high.z, v.z);
    }
    for (size_t i = 0; i < mesh->face_count; ++i) {
        tvdb_triangle f = mesh->faces[i];
        p->normals[i] =
            mesh_normal(mesh->vertices[f.v0], mesh->vertices[f.v1], mesh->vertices[f.v2]);
    }
    if (tvdb_bvh_init(&p->bvh, mesh, mesh->face_count) != 0)
        goto oom;
    *out = p;
    return TVDB_OK;
oom:
    tvdb_mesh_sdf_destroy(p);
    return mesh_error(err, TVDB_ERROR_OUT_OF_MEMORY, "mesh acceleration allocation failed");
}
void tvdb_mesh_sdf_destroy(tvdb_mesh_sdf_t *p) {
    if (p) {
        tvdb_bvh_free(&p->bvh);
        free(p->normals);
        free(p);
    }
}
void tvdb_mesh_sdf_info(const tvdb_mesh_sdf_t *p, tvdb_mesh_sdf_info_t *out) {
    if (p && out) {
        out->faces = p->face_count;
        out->acceleration_bytes =
            sizeof(*p) +
            p->face_count * (sizeof(tvdb_vec3f) + sizeof(int32_t) + 6 * sizeof(float)) +
            (size_t)p->bvh.node_capacity * sizeof(tvdb_bvh_node_t);
    }
}
tvdb_status_t tvdb_mesh_sdf_distance(const tvdb_mesh_sdf_t *p, const double point[3],
                                    double *distance, tvdb_error_t *err) {
    if (!p || !point || !distance || p->mesh->vertices != p->vertices ||
        p->mesh->faces != p->faces || p->mesh->vertex_count != p->vertex_count ||
        p->mesh->face_count != p->face_count)
        return mesh_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid distance workspace");
    for (int a = 0; a < 3; ++a)
        if (!isfinite(point[a]) || fabs(point[a]) > FLT_MAX)
            return mesh_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid distance point");
    tvdb_vec3f q = {(float)point[0], (float)point[1], (float)point[2]}, cp, normal;
    int32_t face;
    tvdb_bvh_closest(&p->bvh, p->mesh, p->normals, q, INFINITY, -1, &cp, &normal, &face);
    double d = hypot(hypot(point[0] - cp.x, point[1] - cp.y), point[2] - cp.z);
    if (face < 0 || !isfinite(d))
        return mesh_error(err, TVDB_ERROR_INVALID_DATA, "distance query failed");
    *distance = d;
    return TVDB_OK;
}
static tvdb_status_t mesh_lattice(const tvdb_mesh_sdf_t *p, float h, float band, tvdb_dense_grid *g,
                                  size_t *bytes, tvdb_error_t *err) {
    if (!isfinite(h) || h <= 0 || !isfinite(band) || band <= 0)
        return mesh_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid mesh SDF spacing or band");
    tvdb_vec3f low = p->low, high = p->high;
    low.x -= band;
    low.y -= band;
    low.z -= band;
    high.x += band;
    high.y += band;
    high.z += band;
    const float lo[3] = {low.x, low.y, low.z}, hi[3] = {high.x, high.y, high.z};
    int dim[3];
    for (int a = 0; a < 3; ++a) {
        float extent = hi[a] - lo[a], ratio = extent / h;
        double n = ceil((double)ratio);
        if (!isfinite(lo[a]) || !isfinite(hi[a]) || !isfinite(n) || n > TVDB_MAX_GRID_DIM)
            return mesh_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                              "mesh SDF extent exceeds supported lattice");
        dim[a] = n < 1 ? 1 : (int)n;
    }
    *g = (tvdb_dense_grid){dim[0], dim[1], dim[2], low.x, low.y, low.z, h, NULL};
    if (!tvdb_grid_bytes(dim[0], dim[1], dim[2], sizeof(float), bytes))
        return mesh_error(err, TVDB_ERROR_INVALID_ARGUMENT, "mesh SDF allocation overflow");
    return TVDB_OK;
}
typedef struct {
    const tvdb_mesh_sdf_t *p;
    tvdb_dense_grid *grid;
    float band;
    atomic_int bad;
} mesh_fill_job;
static void mesh_fill_rows(size_t begin, size_t end, void *user) {
    mesh_fill_job *j = user;
    const tvdb_mesh_sdf_t *p = j->p;
    const tvdb_triangle_mesh *mesh = p->mesh;
    tvdb_dense_grid *g = j->grid;
    for (size_t row = begin; row < end; ++row) {
        int y = (int)(row % g->ny), z = (int)(row / g->ny);
        float seed = INFINITY;
        int32_t seed_face = -1;
        for (int x = 0; x < g->nx; ++x) {
            tvdb_vec3f point = mesh_position(g, x, y, z), cp, n;
            int32_t face;
            float dsq =
                tvdb_bvh_closest(&p->bvh, mesh, p->normals, point, seed, seed_face, &cp, &n, &face);
            seed = dsq;
            seed_face = face;
            float direction = dot_c(sub_c(point, cp), n);
            if (face < 0 || !isfinite(dsq) || !isfinite(direction))
                atomic_store_explicit(&j->bad, 1, memory_order_relaxed);
            float distance = sqrtf(dsq), sign = direction >= 0 ? 1 : -1;
            float v = sign * distance;
            if (v > j->band)
                v = j->band;
            if (v < -j->band)
                v = -j->band;
            if (!isfinite(v))
                atomic_store_explicit(&j->bad, 1, memory_order_relaxed);
            g->data[row * (size_t)g->nx + x] = v;
        }
    }
}
tvdb_status_t tvdb_mesh_sdf_generate(const tvdb_mesh_sdf_t *p, float h, float band,
                                     tvdb_thread_pool_t *pool, tvdb_dense_grid *out,
                                     tvdb_arena_allocator_t *arena, tvdb_error_t *err) {
    if (!p || !out || p->mesh->vertices != p->vertices || p->mesh->faces != p->faces ||
        p->mesh->vertex_count != p->vertex_count || p->mesh->face_count != p->face_count)
        return mesh_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid or changed mesh workspace");
    size_t old_bytes = 0;
    if (out->data && !tvdb_grid_bytes(out->nx, out->ny, out->nz, sizeof(float), &old_bytes))
        return mesh_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid owning output grid");
    if (tvdb_buffers_overlap(out->data, old_bytes, p->vertices,
                             p->vertex_count * sizeof(tvdb_vec3f)) ||
        tvdb_buffers_overlap(out->data, old_bytes, p->faces, p->face_count * sizeof(tvdb_triangle)))
        return mesh_error(err, TVDB_ERROR_INVALID_ARGUMENT, "mesh and output grid overlap");
    tvdb_dense_grid g = {0};
    size_t bytes;
    tvdb_status_t st = mesh_lattice(p, h, band, &g, &bytes, err);
    if (st != TVDB_OK)
        return st;
    size_t mark = arena ? arena->current_offset : 0;
    tvdb_thread_pool_t *owned = NULL;
    if (arena) {
        size_t offset = mark;
        void *destination = mesh_arena_destination(arena, &offset, bytes);
        if (!destination)
            return mesh_error(err, TVDB_ERROR_OUT_OF_MEMORY, "mesh SDF arena exhausted");
        if (tvdb_buffers_overlap(destination, bytes, p->vertices,
                                 p->vertex_count * sizeof(tvdb_vec3f)) ||
            tvdb_buffers_overlap(destination, bytes, p->faces,
                                 p->face_count * sizeof(tvdb_triangle)) ||
            tvdb_buffers_overlap(destination, bytes, out->data, old_bytes))
            return mesh_error(err, TVDB_ERROR_INVALID_ARGUMENT, "mesh SDF arena storage overlaps");
    }
    g.data = arena_alloc_wrapper(arena, bytes);
    if (!g.data)
        return mesh_error(err, TVDB_ERROR_OUT_OF_MEMORY, "mesh SDF output allocation failed");
    size_t frame_bytes;
    if (!mesh_grid_frame(&g, &frame_bytes)) {
        st = mesh_error(err, TVDB_ERROR_INVALID_ARGUMENT, "mesh SDF world coordinates overflow");
        goto done;
    }
    if (!pool) {
        st = mesh_pool(bytes / sizeof(float), (size_t)g.ny * g.nz, &owned, err);
        if (st != TVDB_OK)
            goto done;
        pool = owned;
    }
    /* atomic_init, not ATOMIC_VAR_INIT: the macro is deprecated since C17 and
     * MSVC's <stdatomic.h> does not define it. */
    mesh_fill_job job = {p, &g, band};
    atomic_init(&job.bad, 0);
    st = tvdb_thread_pool_for(pool, 0, (size_t)g.ny * g.nz, 4, mesh_fill_rows, &job, err);
    if (st == TVDB_OK && atomic_load_explicit(&job.bad, memory_order_relaxed))
        st = mesh_error(err, TVDB_ERROR_INVALID_DATA, "mesh distance arithmetic failed");
    if (st == TVDB_OK) {
        if (!arena)
            free(out->data);
        *out = g;
        g.data = NULL;
    }
done:
    tvdb_thread_pool_destroy(owned);
    if (g.data) {
        if (arena)
            arena->current_offset = mark;
        else
            free(g.data);
    }
    return st;
}
tvdb_status_t tvdb_mesh_to_sdf_ex(const tvdb_triangle_mesh *mesh, float h, float band,
                                  tvdb_thread_pool_t *pool, tvdb_dense_grid *out,
                                  tvdb_arena_allocator_t *arena, tvdb_error_t *err) {
    if (!out || !isfinite(h) || h <= 0 || !isfinite(band) || band <= 0)
        return mesh_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid mesh SDF arguments");
    tvdb_mesh_sdf_t *p = NULL;
    tvdb_status_t st = tvdb_mesh_sdf_create(mesh, &p, err);
    if (st == TVDB_OK)
        st = tvdb_mesh_sdf_generate(p, h, band, pool, out, arena, err);
    tvdb_mesh_sdf_destroy(p);
    return st;
}
bool tvdb_mesh_to_sdf(const tvdb_triangle_mesh *mesh, float h, float band, tvdb_dense_grid *out,
                      tvdb_arena_allocator_t *arena) {
    if (!out)
        return false;
    tvdb_dense_grid generated = {0};
    if (tvdb_mesh_to_sdf_ex(mesh, h, band, NULL, &generated, arena, NULL) != TVDB_OK)
        return false;
    *out = generated;
    return true; /* Legacy constructor never reads the old handle. */
}

/* One count record per sample plane; no per-cell compaction array. */
typedef struct {
    size_t vertices, faces;
    int bad;
} mesh_mc_count;
typedef struct {
    const tvdb_dense_grid *g;
    float iso;
    mesh_mc_count *counts;
} mesh_mc_job;
static int mesh_cube_case(const tvdb_dense_grid *g, int x, int y, int z, float iso) {
    int cube = 0;
    for (int c = 0; c < 8; ++c) {
        size_t i = (size_t)voxel_idx_c(g->nx, g->ny, x + MC_CORNER_OFFSETS[c][0],
                                       y + MC_CORNER_OFFSETS[c][1], z + MC_CORNER_OFFSETS[c][2]);
        if (g->data[i] < iso)
            cube |= 1 << c;
    }
    return cube;
}
static void mesh_mc_count_planes(size_t begin, size_t end, void *user) {
    mesh_mc_job *j = user;
    const tvdb_dense_grid *g = j->g;
    size_t plane = (size_t)g->nx * g->ny;
    for (size_t z = begin; z < end; ++z) {
        mesh_mc_count count = {0};
        for (int y = 0; y < g->ny; ++y)
            for (int x = 0; x < g->nx; ++x) {
                size_t at = z * plane + (size_t)y * g->nx + x;
                float v = g->data[at];
                int below = v < j->iso;
                if (!isfinite(v))
                    count.bad = 1;
                if (x + 1 < g->nx && below != (g->data[at + 1] < j->iso))
                    ++count.vertices;
                if (y + 1 < g->ny && below != (g->data[at + g->nx] < j->iso))
                    ++count.vertices;
                if (z + 1 < (size_t)g->nz && below != (g->data[at + plane] < j->iso))
                    ++count.vertices;
                if (x + 1 < g->nx && y + 1 < g->ny && z + 1 < (size_t)g->nz) {
                    const int *tri = MC_TRI_TABLE[mesh_cube_case(g, x, y, (int)z, j->iso)];
                    for (int t = 0; t < 16 && tri[t] >= 0; t += 3)
                        ++count.faces;
                }
            }
        j->counts[z] = count;
    }
}
typedef struct {
    uint64_t key;
    uint32_t value;
} mesh_edge_entry;
typedef struct {
    uint32_t *rolling;
    mesh_edge_entry *hash;
    size_t xplane, yplane, zplane, mask;
} mesh_edge_cache;
static uint64_t mesh_edge_hash(uint64_t x) {
    x ^= x >> 33;
    x *= UINT64_C(0xff51afd7ed558ccd);
    x ^= x >> 33;
    x *= UINT64_C(0xc4ceb9fe1a85ec53);
    return x ^ (x >> 33);
}
static uint32_t *mesh_edge_slot(mesh_edge_cache *cache, const tvdb_dense_grid *g, int x, int y,
                                int z, int axis) {
    if (cache->rolling) {
        size_t i = axis == 0   ? (z & 1) * cache->xplane + (size_t)y * (g->nx - 1) + x
                   : axis == 1 ? 2 * cache->xplane + (z & 1) * cache->yplane + (size_t)y * g->nx + x
                               : 2 * (cache->xplane + cache->yplane) + (size_t)y * g->nx + x;
        return cache->rolling + i;
    }
    uint64_t key = 3 * voxel_idx_c(g->nx, g->ny, x, y, z) + (unsigned)axis + 1;
    size_t at = (size_t)mesh_edge_hash(key) & cache->mask;
    while (cache->hash[at].key && cache->hash[at].key != key)
        at = (at + 1) & cache->mask;
    if (!cache->hash[at].key) {
        cache->hash[at].key = key;
        cache->hash[at].value = UINT32_MAX;
    }
    return &cache->hash[at].value;
}
static tvdb_vec3f mesh_edge_interpolate(const tvdb_dense_grid *g, float iso, int x0, int y0, int z0,
                                        int x1, int y1, int z1) {
    float v0 = g->data[voxel_idx_c(g->nx, g->ny, x0, y0, z0)],
          v1 = g->data[voxel_idx_c(g->nx, g->ny, x1, y1, z1)];
    double t = ((double)iso - v0) / ((double)v1 - v0);
    if (t < 0)
        t = 0;
    if (t > 1)
        t = 1;
    tvdb_vec3f a = mesh_position(g, x0, y0, z0), b = mesh_position(g, x1, y1, z1);
    return (tvdb_vec3f){(float)((1 - t) * a.x + t * b.x), (float)((1 - t) * a.y + t * b.y),
                        (float)((1 - t) * a.z + t * b.z)};
}
static void mesh_mc_emit(const tvdb_dense_grid *g, float iso, tvdb_mesh_winding_t winding,
                         mesh_edge_cache *cache, tvdb_triangle_mesh *out) {
    for (int z = 0; z < g->nz - 1; ++z) {
        if (cache->rolling && z) {
            memset(cache->rolling + ((z + 1) & 1) * cache->xplane, 255,
                   cache->xplane * sizeof(uint32_t));
            memset(cache->rolling + 2 * cache->xplane + ((z + 1) & 1) * cache->yplane, 255,
                   cache->yplane * sizeof(uint32_t));
            memset(cache->rolling + 2 * (cache->xplane + cache->yplane), 255,
                   cache->zplane * sizeof(uint32_t));
        }
        for (int y = 0; y < g->ny - 1; ++y)
            for (int x = 0; x < g->nx - 1; ++x) {
                int cube = mesh_cube_case(g, x, y, z, iso), edges = MC_EDGE_TABLE[cube];
                if (!edges)
                    continue;
                uint32_t vertex[12] = {0};
                for (int e = 0; e < 12; ++e)
                    if (edges & (1 << e)) {
                        int a = MC_EDGE_VERTS[e][0], b = MC_EDGE_VERTS[e][1];
                        int x0 = x + MC_CORNER_OFFSETS[a][0], y0 = y + MC_CORNER_OFFSETS[a][1],
                            z0 = z + MC_CORNER_OFFSETS[a][2];
                        int x1 = x + MC_CORNER_OFFSETS[b][0], y1 = y + MC_CORNER_OFFSETS[b][1],
                            z1 = z + MC_CORNER_OFFSETS[b][2];
                        int axis = x0 != x1 ? 0 : y0 != y1 ? 1 : 2;
                        uint32_t *slot = mesh_edge_slot(cache, g, x0 < x1 ? x0 : x1,
                                                        y0 < y1 ? y0 : y1, z0 < z1 ? z0 : z1, axis);
                        if (*slot == UINT32_MAX) {
                            *slot = (uint32_t)out->vertex_count;
                            out->vertices[out->vertex_count++] =
                                mesh_edge_interpolate(g, iso, x0, y0, z0, x1, y1, z1);
                        }
                        vertex[e] = *slot;
                    }
                const int *tri = MC_TRI_TABLE[cube];
                for (int t = 0; t < 16 && tri[t] >= 0; t += 3)
                    out->faces[out->face_count++] = (tvdb_triangle){
                        vertex[tri[t]],
                        vertex[tri[t + (winding == TVDB_MESH_WINDING_OUTWARD ? 2 : 1)]],
                        vertex[tri[t + (winding == TVDB_MESH_WINDING_OUTWARD ? 1 : 2)]]};
            }
    }
}
tvdb_status_t tvdb_sdf_to_mesh_ex(const tvdb_dense_grid *g, float iso, tvdb_mesh_winding_t winding,
                                  tvdb_thread_pool_t *pool, tvdb_triangle_mesh *out,
                                  tvdb_arena_allocator_t *arena, tvdb_error_t *err) {
    size_t grid_bytes, vbytes, fbytes;
    if (!out || (winding != TVDB_MESH_WINDING_TABLE && winding != TVDB_MESH_WINDING_OUTWARD) ||
        !isfinite(iso) || !mesh_grid_frame(g, &grid_bytes) || g->nx < 2 || g->ny < 2 || g->nz < 2 ||
        out->vertex_count > out->vertex_capacity || out->face_count > out->face_capacity ||
        out->vertex_count > UINT32_MAX || (out->vertex_capacity && !out->vertices) ||
        (out->face_capacity && !out->faces) ||
        !tvdb_size_mul(out->vertex_capacity, sizeof(tvdb_vec3f), &vbytes) ||
        !tvdb_size_mul(out->face_capacity, sizeof(tvdb_triangle), &fbytes))
        return mesh_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                          "invalid marching cubes grid or owning output");
    if (tvdb_buffers_overlap(g->data, grid_bytes, out->vertices, vbytes) ||
        tvdb_buffers_overlap(g->data, grid_bytes, out->faces, fbytes) ||
        tvdb_buffers_overlap(out->vertices, vbytes, out->faces, fbytes))
        return mesh_error(err, TVDB_ERROR_INVALID_ARGUMENT, "marching cubes storage overlaps");
    tvdb_thread_pool_t *owned = NULL;
    mesh_mc_count *counts = NULL;
    mesh_edge_cache cache = {0};
    tvdb_triangle_mesh result = *out;
    tvdb_status_t st = TVDB_OK;
    size_t mark = arena ? arena->current_offset : 0, count_bytes;
    if (!tvdb_size_mul((size_t)g->nz, sizeof(mesh_mc_count), &count_bytes))
        return mesh_error(err, TVDB_ERROR_INVALID_ARGUMENT, "marching cubes count overflow");
    counts = malloc(count_bytes);
    if (!counts)
        goto oom;
    if (!pool) {
        st = mesh_pool(grid_bytes / sizeof(float), (size_t)g->nz, &owned, err);
        if (st != TVDB_OK)
            goto done;
        pool = owned;
    }
    mesh_mc_job job = {g, iso, counts};
    st = tvdb_thread_pool_for(pool, 0, (size_t)g->nz, 1, mesh_mc_count_planes, &job, err);
    if (st != TVDB_OK)
        goto done;
    size_t nv = 0, nf = 0;
    for (int z = 0; z < g->nz; ++z) {
        if (counts[z].bad) {
            st = mesh_error(err, TVDB_ERROR_INVALID_DATA, "nonfinite marching cubes sample");
            goto done;
        }
        if (counts[z].vertices > SIZE_MAX - nv || counts[z].faces > SIZE_MAX - nf)
            goto overflow;
        nv += counts[z].vertices;
        nf += counts[z].faces;
    }
    if (nv > UINT32_MAX - out->vertex_count || nf > SIZE_MAX - out->face_count)
        goto overflow;
    if (!nv) {
        st = TVDB_OK;
        goto done;
    }
    size_t total_v = out->vertex_count + nv, total_f = out->face_count + nf;
    if (!tvdb_size_mul(total_v, sizeof(tvdb_vec3f), &vbytes) ||
        !tvdb_size_mul(total_f, sizeof(tvdb_triangle), &fbytes))
        goto overflow;
    if (arena) {
        size_t offset = mark;
        void *v = total_v > out->vertex_capacity ? mesh_arena_destination(arena, &offset, vbytes)
                                                 : out->vertices;
        void *f = total_f > out->face_capacity ? mesh_arena_destination(arena, &offset, fbytes)
                                               : out->faces;
        if (!v || !f)
            goto oom;
        size_t old_vbytes = out->vertex_capacity * sizeof(tvdb_vec3f);
        size_t old_fbytes = out->face_capacity * sizeof(tvdb_triangle);
        if (tvdb_buffers_overlap(v, vbytes, g->data, grid_bytes) ||
            tvdb_buffers_overlap(f, fbytes, g->data, grid_bytes) ||
            tvdb_buffers_overlap(v, vbytes, f, fbytes) ||
            (v != out->vertices && tvdb_buffers_overlap(v, vbytes, out->vertices, old_vbytes)) ||
            (f != out->faces && tvdb_buffers_overlap(f, fbytes, out->faces, old_fbytes)) ||
            tvdb_buffers_overlap(v, vbytes, out->faces, old_fbytes) ||
            tvdb_buffers_overlap(f, fbytes, out->vertices, old_vbytes)) {
            st = mesh_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                            "marching cubes arena storage overlaps");
            goto done;
        }
    }
    cache.xplane = (size_t)(g->nx - 1) * g->ny;
    cache.yplane = (size_t)g->nx * (g->ny - 1);
    cache.zplane = (size_t)g->nx * g->ny;
    size_t rolling_bytes, hash_bytes = SIZE_MAX, capacity = 0;
    size_t plane_edges = 2 * (cache.xplane + cache.yplane) + cache.zplane;
    if (!tvdb_size_mul(plane_edges, sizeof(uint32_t), &rolling_bytes))
        goto overflow;
    if (tvdb_hash_capacity(nv, 2, &capacity))
        tvdb_size_mul(capacity, sizeof(mesh_edge_entry), &hash_bytes);
    if (hash_bytes < rolling_bytes) {
        cache.hash = calloc(capacity, sizeof(mesh_edge_entry));
        cache.mask = capacity - 1;
        if (!cache.hash)
            goto oom;
    } else {
        cache.rolling = malloc(rolling_bytes);
        if (!cache.rolling)
            goto oom;
        memset(cache.rolling, 255, rolling_bytes);
    }
    if (total_v > result.vertex_capacity) {
        result.vertices = arena_alloc_wrapper(arena, vbytes);
        if (!result.vertices)
            goto oom;
        result.vertex_capacity = total_v;
        if (out->vertex_count)
            memcpy(result.vertices, out->vertices, out->vertex_count * sizeof(tvdb_vec3f));
    }
    if (total_f > result.face_capacity) {
        result.faces = arena_alloc_wrapper(arena, fbytes);
        if (!result.faces)
            goto oom;
        result.face_capacity = total_f;
        if (out->face_count)
            memcpy(result.faces, out->faces, out->face_count * sizeof(tvdb_triangle));
    }
    mesh_mc_emit(g, iso, winding, &cache, &result);
    assert(result.vertex_count == total_v && result.face_count == total_f);
    if (!arena) {
        if (result.vertices != out->vertices)
            free(out->vertices);
        if (result.faces != out->faces)
            free(out->faces);
    }
    *out = result;
    goto done;
overflow:
    st = mesh_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                    "marching cubes size exceeds index or allocation limits");
    goto done;
oom:
    st = mesh_error(err, TVDB_ERROR_OUT_OF_MEMORY, "marching cubes allocation failed");
done:
    if (st != TVDB_OK) {
        if (arena)
            arena->current_offset = mark;
        else {
            if (result.vertices != out->vertices)
                free(result.vertices);
            if (result.faces != out->faces)
                free(result.faces);
        }
    }
    free(counts);
    free(cache.rolling);
    free(cache.hash);
    tvdb_thread_pool_destroy(owned);
    return st;
}
bool tvdb_sdf_to_mesh(const tvdb_dense_grid *g, float iso, tvdb_triangle_mesh *out,
                      tvdb_arena_allocator_t *arena) {
    return tvdb_sdf_to_mesh_ex(g, iso, TVDB_MESH_WINDING_TABLE, NULL, out, arena, NULL) == TVDB_OK;
}

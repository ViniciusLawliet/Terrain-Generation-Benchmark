#include "terrain_openmp.h"
#include "mc_tables.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <omp.h>

typedef struct {
    Vec3 *data;
    size_t count;
    size_t capacity;
    size_t _pad[5];
} ThreadTriBuffer;

static inline size_t field_index(int i, int j, int k, int Nx, int Ny) {
    return (size_t)k * (size_t)Nx * (size_t)Ny + (size_t)j * (size_t)Nx + (size_t)i;
}

static void tribuffer_reserve(ThreadTriBuffer *tb, size_t extra) {
    if (tb->count + extra <= tb->capacity) return;
    size_t newcap = tb->capacity ? tb->capacity * 2 : 1536;
    while (newcap < tb->count + extra) newcap *= 2;
    tb->data = (Vec3 *)realloc(tb->data, newcap * sizeof(Vec3));
    tb->capacity = newcap;
}

static inline void tribuffer_push_triangle(ThreadTriBuffer *tb, Vec3 a, Vec3 b, Vec3 c) {
    tribuffer_reserve(tb, 3);
    tb->data[tb->count++] = a;
    tb->data[tb->count++] = b;
    tb->data[tb->count++] = c;
}

static void mc_process_cell(const float *field, int Nx, int Ny, int i, int j, int k,
                             float isovalue, ThreadTriBuffer *tb) {
    float cornerVal[8];
    Vec3 cornerPos[8];
    int idx = 0;

    for (int c = 0; c < 8; c++) {
        int ci = i + vertexOffset[c][0];
        int cj = j + vertexOffset[c][1];
        int ck = k + vertexOffset[c][2];
        cornerVal[c] = field[field_index(ci, cj, ck, Nx, Ny)];
        cornerPos[c].x = (float)ci;
        cornerPos[c].y = (float)cj;
        cornerPos[c].z = (float)ck;
        if (cornerVal[c] >= isovalue) idx |= (1 << c);
    }

    int flags = edgeTable[idx];
    if (flags == 0) return;

    Vec3 edgeVertex[12];
    for (int e = 0; e < 12; e++) {
        if (!(flags & (1 << e))) continue;
        int v0 = edgeConnection[e][0];
        int v1 = edgeConnection[e][1];
        float val0 = cornerVal[v0];
        float val1 = cornerVal[v1];
        float denom = val1 - val0;
        float t = (fabsf(denom) > 1e-6f) ? (isovalue - val0) / denom : 0.5f;
        edgeVertex[e].x = cornerPos[v0].x + t * (cornerPos[v1].x - cornerPos[v0].x);
        edgeVertex[e].y = cornerPos[v0].y + t * (cornerPos[v1].y - cornerPos[v0].y);
        edgeVertex[e].z = cornerPos[v0].z + t * (cornerPos[v1].z - cornerPos[v0].z);
    }

    const int *tri = triTable[idx];
    for (int t = 0; t < 5; t++) {
        int a = tri[3 * t];
        if (a < 0) break;
        int b = tri[3 * t + 1];
        int c2 = tri[3 * t + 2];
        tribuffer_push_triangle(tb, edgeVertex[a], edgeVertex[b], edgeVertex[c2]);
    }
}

TriMesh marching_cubes_run(const float *field, int Nx, int Ny, int Nz, float isovalue, int num_threads) {
    int nthreads = num_threads > 0 ? num_threads : omp_get_max_threads();
    omp_set_num_threads(nthreads);

    ThreadTriBuffer *buffers = (ThreadTriBuffer *)aligned_alloc(
        64, (size_t)nthreads * sizeof(ThreadTriBuffer));
    memset(buffers, 0, (size_t)nthreads * sizeof(ThreadTriBuffer));

    int cells_x = Nx - 1;
    int cells_y = Ny - 1;
    int cells_z = Nz - 1;

    #pragma omp parallel
    {
        int tid = omp_get_thread_num();
        ThreadTriBuffer *tb = &buffers[tid];

        #pragma omp for collapse(3) schedule(dynamic, 64)
        for (int k = 0; k < cells_z; k++) {
            for (int j = 0; j < cells_y; j++) {
                for (int i = 0; i < cells_x; i++) {
                    mc_process_cell(field, Nx, Ny, i, j, k, isovalue, tb);
                }
            }
        }
    }

    size_t total = 0;
    for (int t = 0; t < nthreads; t++) total += buffers[t].count;

    TriMesh mesh;
    mesh.vertices = total ? (Vec3 *)malloc(total * sizeof(Vec3)) : NULL;
    mesh.vertex_count = total;
    mesh.triangle_count = total / 3;

    size_t offset = 0;
    for (int t = 0; t < nthreads; t++) {
        if (buffers[t].count) {
            memcpy(mesh.vertices + offset, buffers[t].data, buffers[t].count * sizeof(Vec3));
            offset += buffers[t].count;
        }
        free(buffers[t].data);
    }
    free(buffers);

    return mesh;
}

void trimesh_free(TriMesh *mesh) {
    free(mesh->vertices);
    mesh->vertices = NULL;
    mesh->vertex_count = 0;
    mesh->triangle_count = 0;
}

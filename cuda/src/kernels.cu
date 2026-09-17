#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <cuda_runtime.h>

#include "terrain_cuda.h"
#include "mc_tables.h"

#define CUDA_CHECK(call) do { \
    cudaError_t err__ = (call); \
    if (err__ != cudaSuccess) { \
        fprintf(stderr, "CUDA error at %s:%d: %s\n", __FILE__, __LINE__, cudaGetErrorString(err__)); \
        exit(1); \
    } \
} while (0)

#define CEIL_DIV(a, b) (((a) + (b) - 1) / (b))
#define MC_BLOCK_THREADS 256

__constant__ int d_perm[512];
__constant__ int d_edgeTable[256];
__constant__ int d_triTable[256][16];
__constant__ int d_vertexOffset[8][3];
__constant__ int d_edgeConnection[12][2];
__constant__ int d_numVertsTable[256];

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void cuda_check_device(void) {
    int count = 0;
    CUDA_CHECK(cudaGetDeviceCount(&count));
    if (count == 0) {
        fprintf(stderr, "Error: no CUDA device found\n");
        exit(1);
    }
    cudaDeviceProp prop;
    CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
    fprintf(stderr, "GPU: %s (compute capability %d.%d)\n", prop.name, prop.major, prop.minor);
    CUDA_CHECK(cudaSetDevice(0));
}

void cuda_upload_perlin_permutation(const int *perm512) {
    CUDA_CHECK(cudaMemcpyToSymbol(d_perm, perm512, 512 * sizeof(int)));
}

void cuda_upload_mc_tables(void) {
    static int numVerts[256];
    for (int r = 0; r < 256; r++) {
        int n = 0;
        for (int t = 0; t < 5; t++) {
            if (triTable[r][3 * t] < 0) break;
            n += 3;
        }
        numVerts[r] = n;
    }

    CUDA_CHECK(cudaMemcpyToSymbol(d_edgeTable, edgeTable, sizeof(edgeTable)));
    CUDA_CHECK(cudaMemcpyToSymbol(d_triTable, triTable, sizeof(triTable)));
    CUDA_CHECK(cudaMemcpyToSymbol(d_vertexOffset, vertexOffset, sizeof(vertexOffset)));
    CUDA_CHECK(cudaMemcpyToSymbol(d_edgeConnection, edgeConnection, sizeof(edgeConnection)));
    CUDA_CHECK(cudaMemcpyToSymbol(d_numVertsTable, numVerts, sizeof(numVerts)));
}

__device__ __forceinline__ float perlin_fade_device(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

__device__ __forceinline__ float perlin_lerp_device(float t, float a, float b) {
    return a + t * (b - a);
}

__device__ __forceinline__ float perlin_grad_device(int hash, float x, float y, float z) {
    int h = hash & 15;
    float u = h < 8 ? x : y;
    float v = h < 4 ? y : ((h == 12 || h == 14) ? x : z);
    return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}

__device__ float perlin_noise3_device(float x, float y, float z) {
    float xf = floorf(x);
    float yf = floorf(y);
    float zf = floorf(z);

    int X = ((int)xf) & 255;
    int Y = ((int)yf) & 255;
    int Z = ((int)zf) & 255;

    x -= xf;
    y -= yf;
    z -= zf;

    float u = perlin_fade_device(x);
    float v = perlin_fade_device(y);
    float w = perlin_fade_device(z);

    int A  = d_perm[X] + Y,  AA = d_perm[A] + Z,  AB = d_perm[A + 1] + Z;
    int B  = d_perm[X + 1] + Y, BA = d_perm[B] + Z, BB = d_perm[B + 1] + Z;

    return perlin_lerp_device(w,
        perlin_lerp_device(v,
            perlin_lerp_device(u, perlin_grad_device(d_perm[AA],     x,     y,     z),
                                   perlin_grad_device(d_perm[BA],     x - 1, y,     z)),
            perlin_lerp_device(u, perlin_grad_device(d_perm[AB],     x,     y - 1, z),
                                   perlin_grad_device(d_perm[BB],     x - 1, y - 1, z))),
        perlin_lerp_device(v,
            perlin_lerp_device(u, perlin_grad_device(d_perm[AA + 1], x,     y,     z - 1),
                                   perlin_grad_device(d_perm[BA + 1], x - 1, y,     z - 1)),
            perlin_lerp_device(u, perlin_grad_device(d_perm[AB + 1], x,     y - 1, z - 1),
                                   perlin_grad_device(d_perm[BB + 1], x - 1, y - 1, z - 1))));
}

__global__ void heightmap_kernel(float *heightmap, int Nx, int Nz,
                                  float frequency, float base_height, float amplitude) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    int k = blockIdx.y * blockDim.y + threadIdx.y;
    if (i >= Nx || k >= Nz) return;

    float x = (float)i * frequency;
    float z = (float)k * frequency;
    float h = perlin_noise3_device(x, 0.0f, z);
    heightmap[(size_t)k * Nx + i] = base_height + h * amplitude;
}

int cuda_generate_heightmap(float **d_heightmap_out, int Nx, int Nz,
                             float frequency, float base_height, float amplitude,
                             double *time_seconds) {
    size_t heightmap_elems = (size_t)Nx * (size_t)Nz;

    float *d_heightmap = NULL;
    CUDA_CHECK(cudaMalloc((void **)&d_heightmap, heightmap_elems * sizeof(float)));

    double t0 = now_seconds();

    dim3 block2d(16, 16);
    dim3 grid2d(CEIL_DIV(Nx, block2d.x), CEIL_DIV(Nz, block2d.y));
    heightmap_kernel<<<grid2d, block2d>>>(d_heightmap, Nx, Nz, frequency, base_height, amplitude);
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cudaDeviceSynchronize());

    double t1 = now_seconds();
    *time_seconds = t1 - t0;

    *d_heightmap_out = d_heightmap;
    return 0;
}

__device__ __forceinline__ void linear_to_ijk(size_t linear, int cellsX, int cellsY, int *i, int *j, int *k) {
    size_t plane = (size_t)cellsX * (size_t)cellsY;
    *k = (int)(linear / plane);
    size_t rem = linear % plane;
    *j = (int)(rem / (size_t)cellsX);
    *i = (int)(rem % (size_t)cellsX);
}

__device__ __forceinline__ Vec3 corner_pos(int i, int j, int k, int c) {
    Vec3 p;
    p.x = (float)(i + d_vertexOffset[c][0]);
    p.y = (float)(j + d_vertexOffset[c][1]);
    p.z = (float)(k + d_vertexOffset[c][2]);
    return p;
}

__device__ __forceinline__ int mc_classify_cell_hm(const float *heightmap, int Nx,
                                                     int i, int j, int k, float isovalue,
                                                     float cornerVal[8]) {
    float h00 = heightmap[(size_t)k * Nx + i];
    float h10 = heightmap[(size_t)k * Nx + (i + 1)];
    float h01 = heightmap[(size_t)(k + 1) * Nx + i];
    float h11 = heightmap[(size_t)(k + 1) * Nx + (i + 1)];

    cornerVal[0] = h00 - (float)j;
    cornerVal[1] = h10 - (float)j;
    cornerVal[2] = h10 - (float)(j + 1);
    cornerVal[3] = h00 - (float)(j + 1);
    cornerVal[4] = h01 - (float)j;
    cornerVal[5] = h11 - (float)j;
    cornerVal[6] = h11 - (float)(j + 1);
    cornerVal[7] = h01 - (float)(j + 1);

    int idx = 0;
    for (int c = 0; c < 8; c++) {
        if (cornerVal[c] >= isovalue) idx |= (1 << c);
    }
    return idx;
}

__global__ void mc_count_total_kernel(const float *heightmap, int Nx,
                                       int cellsX, int cellsY, float isovalue,
                                       size_t cells_total,
                                       unsigned long long *d_total_verts) {
    size_t linear = (size_t)blockIdx.x * blockDim.x + threadIdx.x;

    int local_count = 0;
    if (linear < cells_total) {
        int i, j, k;
        linear_to_ijk(linear, cellsX, cellsY, &i, &j, &k);
        float cornerVal[8];
        int idx = mc_classify_cell_hm(heightmap, Nx, i, j, k, isovalue, cornerVal);
        local_count = d_numVertsTable[idx];
    }

    __shared__ int s_count[MC_BLOCK_THREADS];
    s_count[threadIdx.x] = local_count;
    __syncthreads();

    if (threadIdx.x == 0) {
        int block_total = 0;
        for (int t = 0; t < MC_BLOCK_THREADS; t++) block_total += s_count[t];
        if (block_total > 0) {
            atomicAdd(d_total_verts, (unsigned long long)block_total);
        }
    }
}

__global__ void mc_emit_kernel(const float *heightmap, int Nx,
                                int cellsX, int cellsY, float isovalue,
                                size_t cells_total,
                                unsigned long long *d_write_cursor,
                                Vec3 *outVerts) {
    size_t linear = (size_t)blockIdx.x * blockDim.x + threadIdx.x;

    int i = 0, j = 0, k = 0;
    int idx = 0;
    float cornerVal[8];
    int local_count = 0;

    if (linear < cells_total) {
        linear_to_ijk(linear, cellsX, cellsY, &i, &j, &k);
        idx = mc_classify_cell_hm(heightmap, Nx, i, j, k, isovalue, cornerVal);
        local_count = d_numVertsTable[idx];
    }

    __shared__ int s_count[MC_BLOCK_THREADS];
    __shared__ int s_offset[MC_BLOCK_THREADS];
    __shared__ unsigned long long s_block_base;

    s_count[threadIdx.x] = local_count;
    __syncthreads();

    if (threadIdx.x == 0) {
        int running = 0;
        for (int t = 0; t < MC_BLOCK_THREADS; t++) {
            s_offset[t] = running;
            running += s_count[t];
        }
        s_block_base = (running > 0) ? atomicAdd(d_write_cursor, (unsigned long long)running) : 0ULL;
    }
    __syncthreads();

    if (linear >= cells_total || local_count == 0) return;

    int flags = d_edgeTable[idx];

    Vec3 edgeVertex[12];
    for (int e = 0; e < 12; e++) {
        if (!(flags & (1 << e))) continue;
        int v0 = d_edgeConnection[e][0];
        int v1 = d_edgeConnection[e][1];
        float val0 = cornerVal[v0];
        float val1 = cornerVal[v1];
        float denom = val1 - val0;
        float t = (fabsf(denom) > 1e-6f) ? (isovalue - val0) / denom : 0.5f;
        Vec3 p0 = corner_pos(i, j, k, v0);
        Vec3 p1 = corner_pos(i, j, k, v1);
        edgeVertex[e].x = p0.x + t * (p1.x - p0.x);
        edgeVertex[e].y = p0.y + t * (p1.y - p0.y);
        edgeVertex[e].z = p0.z + t * (p1.z - p0.z);
    }

    size_t base = s_block_base + (size_t)s_offset[threadIdx.x];
    int outCount = 0;
    const int *tri = d_triTable[idx];
    for (int t = 0; t < 5; t++) {
        int a = tri[3 * t];
        if (a < 0) break;
        int b = tri[3 * t + 1];
        int c2 = tri[3 * t + 2];
        outVerts[base + outCount++] = edgeVertex[a];
        outVerts[base + outCount++] = edgeVertex[b];
        outVerts[base + outCount++] = edgeVertex[c2];
    }
}

TriMesh cuda_marching_cubes(const float *d_heightmap, int Nx, int Ny, int Nz, float isovalue,
                             double *time_seconds) {
    double t0 = now_seconds();

    int cellsX = Nx - 1, cellsY = Ny - 1, cellsZ = Nz - 1;
    size_t cells_total = (size_t)cellsX * (size_t)cellsY * (size_t)cellsZ;

    int threads = MC_BLOCK_THREADS;
    int blocks = (int)CEIL_DIV(cells_total, (size_t)threads);
    if (blocks < 1) blocks = 1;

    unsigned long long *d_total = NULL;
    CUDA_CHECK(cudaMalloc((void **)&d_total, sizeof(unsigned long long)));
    CUDA_CHECK(cudaMemset(d_total, 0, sizeof(unsigned long long)));

    mc_count_total_kernel<<<blocks, threads>>>(d_heightmap, Nx, cellsX, cellsY, isovalue, cells_total, d_total);
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cudaDeviceSynchronize());

    unsigned long long h_total = 0;
    CUDA_CHECK(cudaMemcpy(&h_total, d_total, sizeof(unsigned long long), cudaMemcpyDeviceToHost));
    cudaFree(d_total);

    TriMesh mesh;
    mesh.vertices = NULL;
    mesh.vertex_count = (size_t)h_total;
    mesh.triangle_count = (size_t)h_total / 3;

    if (h_total > 0) {
        unsigned long long *d_cursor = NULL;
        CUDA_CHECK(cudaMalloc((void **)&d_cursor, sizeof(unsigned long long)));
        CUDA_CHECK(cudaMemset(d_cursor, 0, sizeof(unsigned long long)));

        Vec3 *d_verts = NULL;
        CUDA_CHECK(cudaMalloc((void **)&d_verts, (size_t)h_total * sizeof(Vec3)));

        mc_emit_kernel<<<blocks, threads>>>(d_heightmap, Nx, cellsX, cellsY, isovalue,
                                             cells_total, d_cursor, d_verts);
        CUDA_CHECK(cudaGetLastError());
        CUDA_CHECK(cudaDeviceSynchronize());

        mesh.vertices = (Vec3 *)malloc((size_t)h_total * sizeof(Vec3));
        CUDA_CHECK(cudaMemcpy(mesh.vertices, d_verts, (size_t)h_total * sizeof(Vec3), cudaMemcpyDeviceToHost));

        cudaFree(d_verts);
        cudaFree(d_cursor);
    }

    double t1 = now_seconds();
    *time_seconds = t1 - t0;

    return mesh;
}

void cuda_free_heightmap(float *d_heightmap) {
    cudaFree(d_heightmap);
}

void cuda_free_mesh(TriMesh *mesh) {
    free(mesh->vertices);
    mesh->vertices = NULL;
    mesh->vertex_count = 0;
    mesh->triangle_count = 0;
}
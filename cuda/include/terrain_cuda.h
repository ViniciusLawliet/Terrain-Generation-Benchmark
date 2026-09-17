#ifndef TERRAIN_CUDA_H
#define TERRAIN_CUDA_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void cuda_check_device(void);
void cuda_upload_perlin_permutation(const int *perm512);
void cuda_upload_mc_tables(void);

int cuda_generate_heightmap(float **d_heightmap_out, int Nx, int Nz,
                            float frequency, float base_height, float amplitude,
                            double *time_seconds);

TriMesh cuda_marching_cubes(const float *d_heightmap, int Nx, int Nz, int Ny, float isovalue,
                            double *time_seconds);

void cuda_free_heightmap(float *d_heightmap);
void cuda_free_mesh(TriMesh *mesh);

#ifdef __cplusplus
}
#endif

#endif

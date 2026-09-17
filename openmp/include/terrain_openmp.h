#ifndef TERRAIN_OPENMP_H
#define TERRAIN_OPENMP_H

#include "types.h"

TriMesh marching_cubes_run(const float *heightmap, int Nx, int Ny, int Nz, float isovalue, int num_threads);
void trimesh_free(TriMesh *mesh);

#endif

#ifndef TERRAIN_TYPES_H
#define TERRAIN_TYPES_H

#include <stddef.h>

typedef struct {
    float x, y, z;
} Vec3;

typedef struct {
    Vec3 *vertices;
    size_t vertex_count;
    size_t triangle_count;
} TriMesh;

#endif
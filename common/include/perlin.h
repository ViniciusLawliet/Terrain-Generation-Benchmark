#ifndef PERLIN_H
#define PERLIN_H

#include <stdint.h>

typedef struct {
    int p[512];
} PerlinState;

void perlin_init(PerlinState *ps, uint64_t seed);
double perlin_noise3(const PerlinState *ps, double x, double y, double z);

#endif
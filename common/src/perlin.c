#include "perlin.h"
#include <math.h>

static uint64_t splitmix64_next(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void perlin_init(PerlinState *ps, uint64_t seed) {
    int perm[256];
    for (int i = 0; i < 256; i++) perm[i] = i;

    uint64_t state = seed ^ 0x9E3779B97F4A7C15ULL;
    if (state == 0) state = 0xff51afd7ed558ccdULL;

    for (int i = 255; i > 0; i--) {
        uint64_t r = splitmix64_next(&state);
        int j = (int)(r % (uint64_t)(i + 1));
        int tmp = perm[i];
        perm[i] = perm[j];
        perm[j] = tmp;
    }

    for (int i = 0; i < 256; i++) {
        ps->p[i] = perm[i];
        ps->p[i + 256] = perm[i];
    }
}

static double fade(double t) {
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

static double lerpd(double t, double a, double b) {
    return a + t * (b - a);
}

static double grad(int hash, double x, double y, double z) {
    int h = hash & 15;
    double u = h < 8 ? x : y;
    double v = h < 4 ? y : ((h == 12 || h == 14) ? x : z);
    return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}

double perlin_noise3(const PerlinState *ps, double x, double y, double z) {
    const int *p = ps->p;

    double xf = floor(x);
    double yf = floor(y);
    double zf = floor(z);

    int X = (int)xf & 255;
    int Y = (int)yf & 255;
    int Z = (int)zf & 255;

    x -= xf;
    y -= yf;
    z -= zf;

    double u = fade(x);
    double v = fade(y);
    double w = fade(z);

    int A  = p[X] + Y,  AA = p[A] + Z,  AB = p[A + 1] + Z;
    int B  = p[X + 1] + Y, BA = p[B] + Z, BB = p[B + 1] + Z;

    return lerpd(w, lerpd(v, lerpd(u, grad(p[AA],     x,     y,     z),
                                       grad(p[BA],     x - 1, y,     z)),
                              lerpd(u, grad(p[AB],     x,     y - 1, z),
                                       grad(p[BB],     x - 1, y - 1, z))),
                    lerpd(v, lerpd(u, grad(p[AA + 1],  x,     y,     z - 1),
                                       grad(p[BA + 1],  x - 1, y,     z - 1)),
                              lerpd(u, grad(p[AB + 1],  x,     y - 1, z - 1),
                                       grad(p[BB + 1],  x - 1, y - 1, z - 1))));
}
#ifndef RAPL_H
#define RAPL_H

#include <limits.h>

#define RAPL_MAX_DOMAINS 8

typedef struct {
    int count;
    int available;
    char energy_path[RAPL_MAX_DOMAINS][PATH_MAX];
    char name[RAPL_MAX_DOMAINS][64];
    unsigned long long max_range_uj[RAPL_MAX_DOMAINS];
} RaplContext;

void rapl_init(RaplContext *ctx);
int rapl_read(const RaplContext *ctx, unsigned long long *out_uj);
double rapl_energy_joules(const RaplContext *ctx, const unsigned long long *before, const unsigned long long *after);

#endif
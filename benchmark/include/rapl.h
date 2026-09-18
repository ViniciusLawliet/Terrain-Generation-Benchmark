#ifndef BENCH_RAPL_H
#define BENCH_RAPL_H

#define RAPL_MAX_DOMAINS 16

typedef struct {
    char energy_path[RAPL_MAX_DOMAINS][300];
    unsigned long long max_range_uj[RAPL_MAX_DOMAINS];
    int count;
    int available;
} RaplContext;

void rapl_init(RaplContext *ctx);
int rapl_read(const RaplContext *ctx, unsigned long long *out_uj);
double rapl_energy_joules(const RaplContext *ctx, const unsigned long long *before, const unsigned long long *after);

#endif
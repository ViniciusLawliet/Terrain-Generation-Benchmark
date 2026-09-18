#include "rapl.h"
#include <stdio.h>
#include <string.h>

void rapl_init(RaplContext *ctx) {
    ctx->count = 0;
    ctx->available = 0;

    for (int i = 0; i < RAPL_MAX_DOMAINS; i++) {
        char path[300];
        snprintf(path, sizeof(path), "/sys/class/powercap/intel-rapl/intel-rapl:%d/energy_uj", i);
        FILE *f = fopen(path, "r");
        if (!f) break;
        fclose(f);

        char maxpath[300];
        snprintf(maxpath, sizeof(maxpath), "/sys/class/powercap/intel-rapl/intel-rapl:%d/max_energy_range_uj", i);
        unsigned long long maxval = 0;
        FILE *mf = fopen(maxpath, "r");
        if (mf) {
            if (fscanf(mf, "%llu", &maxval) != 1) maxval = 0;
            fclose(mf);
        }

        snprintf(ctx->energy_path[ctx->count], sizeof(ctx->energy_path[0]),
                 "/sys/class/powercap/intel-rapl/intel-rapl:%d/energy_uj", i);
        ctx->max_range_uj[ctx->count] = maxval;
        ctx->count++;
    }

    ctx->available = (ctx->count > 0);
}

int rapl_read(const RaplContext *ctx, unsigned long long *out_uj) {
    int ok = 0;
    for (int i = 0; i < ctx->count; i++) {
        out_uj[i] = 0;
        FILE *f = fopen(ctx->energy_path[i], "r");
        if (f) {
            if (fscanf(f, "%llu", &out_uj[i]) == 1) ok++;
            fclose(f);
        }
    }
    return ok;
}

double rapl_energy_joules(const RaplContext *ctx, const unsigned long long *before, const unsigned long long *after) {
    double total_uj = 0.0;
    for (int i = 0; i < ctx->count; i++) {
        unsigned long long b = before[i];
        unsigned long long a = after[i];
        unsigned long long diff;
        if (a >= b) {
            diff = a - b;
        } else if (ctx->max_range_uj[i] > 0) {
            diff = (ctx->max_range_uj[i] - b) + a;
        } else {
            diff = 0;
        }
        total_uj += (double)diff;
    }
    return total_uj / 1000000.0;
}
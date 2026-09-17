#define _GNU_SOURCE
#include "rapl.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define RAPL_BASE "/sys/class/powercap/intel-rapl"

static int build_path(char *out, size_t out_size, const char *base, const char *file) {
    int n = snprintf(out, out_size, "%s/%s", base, file);
    return (n >= 0 && (size_t)n < out_size) ? 0 : -1;
}

void rapl_init(RaplContext *ctx) {

    memset(ctx, 0, sizeof(*ctx));

    for (int i = 0; i < RAPL_MAX_DOMAINS; ++i) {
        char dirpath[PATH_MAX];
        char energypath[PATH_MAX];
        char maxpath[PATH_MAX];
        char namepath[PATH_MAX];

        int n = snprintf(dirpath, sizeof(dirpath), RAPL_BASE "/intel-rapl:%d", i);

        if (n < 0 || (size_t)n >= sizeof(dirpath))
            break;

        struct stat st;
        if (stat(dirpath, &st) != 0) {
            if (errno == ENOENT)
                break;
            fprintf(stderr, "RAPL: cannot access %s: %s\n",
                    dirpath, strerror(errno));
            break;
        }

        if (build_path(energypath, sizeof(energypath), dirpath, "energy_uj") != 0 ||
            build_path(maxpath, sizeof(maxpath), dirpath, "max_energy_range_uj") != 0 ||
            build_path(namepath, sizeof(namepath), dirpath, "name") != 0)
            break;

        FILE *f = fopen(energypath, "r");
        if (!f)
            break;
        fclose(f);

        char name[64] = "unknown";
        FILE *nf = fopen(namepath, "r");
        if (nf) {
            if (fgets(name, sizeof(name), nf))
                name[strcspn(name, "\r\n")] = '\0';
            fclose(nf);
        }

        unsigned long long maxval = 0;
        FILE *mf = fopen(maxpath, "r");
        if (mf) {
            if (fscanf(mf, "%llu", &maxval) != 1)
                maxval = 0;
            fclose(mf);
        }

        snprintf(ctx->energy_path[ctx->count],
                 sizeof(ctx->energy_path[0]), "%s", energypath);
        snprintf(ctx->name[ctx->count],
                 sizeof(ctx->name[0]), "%s", name);
        ctx->max_range_uj[ctx->count] = maxval;
        ctx->count++;
    }

    ctx->available = ctx->count > 0;

    if (ctx->available) {
        fprintf(stderr, "RAPL: %d top-level package zone(s) available\n", ctx->count);
    }
}

int rapl_read(const RaplContext *ctx, unsigned long long *out_uj) {

    if (!ctx || !ctx->available || !out_uj)
        return 0;

    int ok = 0;

    for (int i = 0; i < ctx->count; ++i) {
        out_uj[i] = 0;

        FILE *f = fopen(ctx->energy_path[i], "r");
        if (!f)
            continue;

        if (fscanf(f, "%llu", &out_uj[i]) == 1)
            ok++;

        fclose(f);
    }

    return ok;
}

double rapl_energy_joules(const RaplContext *ctx, const unsigned long long *before, const unsigned long long *after) {
    
    if (!ctx || !ctx->available || !before || !after)
        return 0.0;

    double total_uj = 0.0;

    /* Sum only top-level package zones; child zones would double-count. */
    for (int i = 0; i < ctx->count; ++i) {
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
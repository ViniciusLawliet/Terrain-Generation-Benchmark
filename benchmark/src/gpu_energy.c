#include "gpu_energy.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef WITH_NVML
#include <nvml.h>

typedef enum {
    GPU_POWER_TOTAL_ENERGY = 0,
    GPU_POWER_SAMPLES_BUFFER = 1
} GpuPowerMethod;

struct GpuEnergyContext {
    nvmlDevice_t device;
    GpuPowerMethod method;

    unsigned long long start_total_mj;
    int start_valid;

    unsigned long long sample_start_us;
    unsigned long long sample_end_us;
    unsigned long long last_seen_us;

    unsigned long long last_sample_ts;
    unsigned int last_sample_mw;
    int last_sample_valid;

    double sampled_energy_j;
    unsigned int sample_count;
    int last_measurement_valid;

    char device_name[NVML_DEVICE_NAME_V2_BUFFER_SIZE];
    char last_error[512];
};

static void append_error(GpuEnergyContext *ctx, const char *where, nvmlReturn_t rc) {
    if (!ctx) return;

    char line[256];
    snprintf(line, sizeof(line), "%s: %s (%d)", where, nvmlErrorString(rc), (int)rc);

    if (ctx->last_error[0] == '\0') {
        snprintf(ctx->last_error, sizeof(ctx->last_error), "%s", line);
    } else {
        size_t used = strlen(ctx->last_error);
        if (used + 2 < sizeof(ctx->last_error)) {
            snprintf(ctx->last_error + used,
                     sizeof(ctx->last_error) - used,
                     "; %s", line);
        }
    }
}

static unsigned long long realtime_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0ULL;

    return (unsigned long long)ts.tv_sec * 1000000ULL +
           (unsigned long long)ts.tv_nsec / 1000ULL;
}

static nvmlReturn_t probe_power_samples(GpuEnergyContext *ctx) {
    nvmlValueType_t value_type = NVML_VALUE_TYPE_UNSIGNED_INT;
    unsigned int count = 0;

    nvmlReturn_t rc = nvmlDeviceGetSamples(
        ctx->device,
        NVML_TOTAL_POWER_SAMPLES,
        0,
        &value_type,
        &count,
        NULL);

    if (rc == NVML_SUCCESS || rc == NVML_ERROR_NOT_FOUND)
        return rc;

    append_error(ctx, "nvmlDeviceGetSamples(probe)", rc);
    return rc;
}

static nvmlReturn_t get_power_samples_since(GpuEnergyContext *ctx,
                                            unsigned long long last_seen_us,
                                            nvmlSample_t **samples_out,
                                            unsigned int *count_out) {
    *samples_out = NULL;
    *count_out = 0;

    nvmlValueType_t value_type = NVML_VALUE_TYPE_UNSIGNED_INT;
    unsigned int count = 0;

    nvmlReturn_t rc = nvmlDeviceGetSamples(
        ctx->device,
        NVML_TOTAL_POWER_SAMPLES,
        last_seen_us,
        &value_type,
        &count,
        NULL);

    if (rc == NVML_ERROR_NOT_FOUND)
        return rc;

    if (rc != NVML_SUCCESS) {
        append_error(ctx, "nvmlDeviceGetSamples(size)", rc);
        return rc;
    }

    if (count == 0)
        return NVML_ERROR_NOT_FOUND;

    for (int attempt = 0; attempt < 3; ++attempt) {
        nvmlSample_t *samples = calloc(count, sizeof(*samples));
        if (!samples)
            return NVML_ERROR_INSUFFICIENT_SIZE;

        unsigned int capacity = count;
        value_type = NVML_VALUE_TYPE_UNSIGNED_INT;

        rc = nvmlDeviceGetSamples(
            ctx->device,
            NVML_TOTAL_POWER_SAMPLES,
            last_seen_us,
            &value_type,
            &capacity,
            samples);

        if (rc == NVML_SUCCESS) {
            *samples_out = samples;
            *count_out = capacity;
            return NVML_SUCCESS;
        }

        free(samples);

        if (rc != NVML_ERROR_INSUFFICIENT_SIZE) {
            append_error(ctx, "nvmlDeviceGetSamples(data)", rc);
            return rc;
        }

        count = capacity;
        if (count == 0)
            return NVML_ERROR_NOT_FOUND;
    }

    append_error(ctx, "nvmlDeviceGetSamples: size changed repeatedly",
                 NVML_ERROR_UNKNOWN);
    return NVML_ERROR_UNKNOWN;
}

static int sample_cmp_timestamp(const void *a, const void *b) {
    const nvmlSample_t *sa = (const nvmlSample_t *)a;
    const nvmlSample_t *sb = (const nvmlSample_t *)b;

    if (sa->timeStamp < sb->timeStamp) return -1;
    if (sa->timeStamp > sb->timeStamp) return 1;
    return 0;
}

static void integrate_between(GpuEnergyContext *ctx,
                              unsigned long long t_us,
                              unsigned int power_mw) {
    if (!ctx || !ctx->last_sample_valid)
        return;

    if (t_us <= ctx->last_sample_ts)
        return;

    unsigned long long start_us = ctx->last_sample_ts;
    unsigned long long end_us = t_us;
    double avg_w = ((double)ctx->last_sample_mw + (double)power_mw) / 2000.0;

    /* Only account for the requested measurement window. */
    if (ctx->sample_start_us > start_us) {
        start_us = ctx->sample_start_us;
        if (end_us <= start_us)
            return;
    }

    if (ctx->sample_end_us != 0ULL && end_us > ctx->sample_end_us)
        end_us = ctx->sample_end_us;

    if (end_us <= start_us)
        return;

    ctx->sampled_energy_j += avg_w * ((double)(end_us - start_us) / 1e6);
    ctx->sample_count++;
}

static void consume_samples(GpuEnergyContext *ctx,
                            nvmlSample_t *samples,
                            unsigned int count) {
    if (!ctx || !samples || count == 0)
        return;

    qsort(samples, count, sizeof(*samples), sample_cmp_timestamp);

    for (unsigned int i = 0; i < count; ++i) {
        unsigned long long ts = samples[i].timeStamp;
        unsigned int mw = samples[i].sampleValue.uiVal;

        if (ts <= ctx->last_seen_us)
            continue;

        /* The first sample establishes the power immediately before/after the
         * measurement boundary.  Subsequent samples form trapezoids. */
        if (!ctx->last_sample_valid) {
            ctx->last_sample_ts = ts;
            ctx->last_sample_mw = mw;
            ctx->last_sample_valid = 1;
            ctx->last_seen_us = ts;
            continue;
        }

        if (ctx->sample_end_us != 0ULL && ts > ctx->sample_end_us) {
            ctx->last_seen_us = ts;
            continue;
        }

        integrate_between(ctx, ts, mw);
        ctx->last_sample_ts = ts;
        ctx->last_sample_mw = mw;
        ctx->last_seen_us = ts;
    }
}

static void drain_samples(GpuEnergyContext *ctx) {
    if (!ctx || ctx->method != GPU_POWER_SAMPLES_BUFFER)
        return;

    nvmlSample_t *samples = NULL;
    unsigned int count = 0;
    nvmlReturn_t rc = get_power_samples_since(ctx, ctx->last_seen_us,
                                               &samples, &count);

    if (rc == NVML_ERROR_NOT_FOUND)
        return;

    if (rc != NVML_SUCCESS) {
        append_error(ctx, "nvmlDeviceGetSamples(drain)", rc);
        free(samples);
        return;
    }

    consume_samples(ctx, samples, count);
    free(samples);
}

static const char *method_name(GpuPowerMethod method) {
    switch (method) {
        case GPU_POWER_TOTAL_ENERGY:
            return "nvml_total_energy_consumption";
        case GPU_POWER_SAMPLES_BUFFER:
            return "nvml_power_samples_buffer";
        default:
            return "unavailable";
    }
}

GpuEnergyContext *gpu_energy_init(int device_index) {
    GpuEnergyContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->last_error[0] = '\0';

    nvmlReturn_t rc = nvmlInit_v2();
    if (rc != NVML_SUCCESS) {
        append_error(ctx, "nvmlInit_v2", rc);
        fprintf(stderr, "GPU NVML error: %s\n", ctx->last_error);
        free(ctx);
        return NULL;
    }

    rc = nvmlDeviceGetHandleByIndex((unsigned int)device_index, &ctx->device);
    if (rc != NVML_SUCCESS) {
        append_error(ctx, "nvmlDeviceGetHandleByIndex", rc);
        fprintf(stderr, "GPU NVML error: %s\n", ctx->last_error);
        nvmlShutdown();
        free(ctx);
        return NULL;
    }

    rc = nvmlDeviceGetName(ctx->device,
                           ctx->device_name,
                           sizeof(ctx->device_name));
    if (rc != NVML_SUCCESS)
        snprintf(ctx->device_name, sizeof(ctx->device_name), "unknown");

    unsigned long long energy_mj = 0;
    rc = nvmlDeviceGetTotalEnergyConsumption(ctx->device, &energy_mj);

    if (rc == NVML_SUCCESS) {
        ctx->method = GPU_POWER_TOTAL_ENERGY;
    } else {
        append_error(ctx, "nvmlDeviceGetTotalEnergyConsumption(probe)", rc);

        nvmlReturn_t sample_rc = probe_power_samples(ctx);
        if (sample_rc == NVML_SUCCESS || sample_rc == NVML_ERROR_NOT_FOUND) {
            ctx->method = GPU_POWER_SAMPLES_BUFFER;
        } else {
            fprintf(stderr,
                    "GPU NVML error: no supported GPU energy telemetry for %s\n",
                    ctx->device_name);
            fprintf(stderr, "  details: %s\n", ctx->last_error);
            nvmlShutdown();
            free(ctx);
            return NULL;
        }
    }

    fprintf(stderr, "GPU energy telemetry: %s (%s)\n",
            ctx->device_name,
            method_name(ctx->method));

    return ctx;
}

void gpu_energy_start(GpuEnergyContext *ctx) {
    if (!ctx)
        return;

    ctx->last_measurement_valid = 0;
    ctx->sample_count = 0;
    ctx->sampled_energy_j = 0.0;
    ctx->start_valid = 0;
    ctx->last_seen_us = 0ULL;
    ctx->last_sample_valid = 0;
    ctx->last_sample_ts = 0ULL;
    ctx->last_sample_mw = 0U;

    if (ctx->method == GPU_POWER_TOTAL_ENERGY) {
        unsigned long long start_mj = 0;
        nvmlReturn_t rc = nvmlDeviceGetTotalEnergyConsumption(
            ctx->device, &start_mj);

        if (rc == NVML_SUCCESS) {
            ctx->start_total_mj = start_mj;
            ctx->start_valid = 1;
        } else {
            append_error(ctx,
                         "nvmlDeviceGetTotalEnergyConsumption(start)",
                         rc);
        }
        return;
    }

    ctx->sample_start_us = realtime_us();
    ctx->sample_end_us = 0ULL;
    ctx->start_valid = (ctx->sample_start_us != 0ULL);

    if (!ctx->start_valid)
        return;

    /* Prime the lastSeen timestamp with the samples already present.  This
     * prevents warm-up/idle samples from being charged to the measurement. */
    nvmlSample_t *samples = NULL;
    unsigned int count = 0;
    nvmlReturn_t rc = get_power_samples_since(ctx, 0ULL, &samples, &count);

    if (rc == NVML_SUCCESS) {
        qsort(samples, count, sizeof(*samples), sample_cmp_timestamp);

        consume_samples(ctx, samples, count);
        free(samples);
    } else if (rc != NVML_ERROR_NOT_FOUND) {
        append_error(ctx, "nvmlDeviceGetSamples(start)", rc);
        free(samples);
        ctx->start_valid = 0;
        return;
    }

    /* If there is already a sample after the start boundary, consume it now. */
    drain_samples(ctx);
}

void gpu_energy_poll(GpuEnergyContext *ctx) {
    if (!ctx || !ctx->start_valid)
        return;

    if (ctx->method == GPU_POWER_SAMPLES_BUFFER)
        drain_samples(ctx);
}

double gpu_energy_stop_joules(GpuEnergyContext *ctx) {
    if (!ctx || !ctx->start_valid)
        return 0.0;

    if (ctx->method == GPU_POWER_TOTAL_ENERGY) {
        unsigned long long end_mj = 0;
        nvmlReturn_t rc = nvmlDeviceGetTotalEnergyConsumption(
            ctx->device, &end_mj);

        if (rc == NVML_SUCCESS && end_mj >= ctx->start_total_mj) {
            ctx->last_measurement_valid = 1;
            return (double)(end_mj - ctx->start_total_mj) / 1000.0;
        }

        if (rc != NVML_SUCCESS)
            append_error(ctx, "nvmlDeviceGetTotalEnergyConsumption(stop)", rc);

        ctx->last_measurement_valid = 0;
        return 0.0;
    }

    if (ctx->method == GPU_POWER_SAMPLES_BUFFER) {
        ctx->sample_end_us = realtime_us();
        if (ctx->sample_end_us == 0ULL || ctx->sample_end_us <= ctx->sample_start_us) {
            ctx->last_measurement_valid = 0;
            return 0.0;
        }

        /* Final drain. No sample with a timestamp beyond the measurement end
         * contributes to the integral. The last observed power is held until
         * the end timestamp. */
        drain_samples(ctx);

        if (ctx->last_sample_valid && ctx->last_sample_ts < ctx->sample_end_us) {
            unsigned long long end_us = ctx->sample_end_us;
            unsigned long long start_us = ctx->last_sample_ts;
            if (start_us < ctx->sample_start_us)
                start_us = ctx->sample_start_us;

            if (end_us > start_us) {
                ctx->sampled_energy_j +=
                    ((double)ctx->last_sample_mw / 1000.0) *
                    ((double)(end_us - start_us) / 1e6);
                ctx->sample_count++;
            }
        }

        ctx->last_measurement_valid =
            isfinite(ctx->sampled_energy_j) && ctx->sample_count >= 2;

        return ctx->last_measurement_valid ? ctx->sampled_energy_j : 0.0;
    }

    return 0.0;
}

int gpu_energy_measurement_valid(GpuEnergyContext *ctx) {
    return ctx ? ctx->last_measurement_valid : 0;
}

const char *gpu_energy_method(GpuEnergyContext *ctx) {
    return ctx ? method_name(ctx->method) : "unavailable";
}

const char *gpu_energy_error(GpuEnergyContext *ctx) {
    if (!ctx || ctx->last_error[0] == '\0')
        return "";
    return ctx->last_error;
}

const char *gpu_energy_device_name(GpuEnergyContext *ctx) {
    if (!ctx || ctx->device_name[0] == '\0')
        return "unavailable";
    return ctx->device_name;
}

void gpu_energy_destroy(GpuEnergyContext *ctx) {
    if (!ctx)
        return;

    nvmlShutdown();
    free(ctx);
}

#else

struct GpuEnergyContext { int unused; };

GpuEnergyContext *gpu_energy_init(int device_index) {
    (void)device_index;
    return NULL;
}

void gpu_energy_start(GpuEnergyContext *ctx) {
    (void)ctx;
}

void gpu_energy_poll(GpuEnergyContext *ctx) {
    (void)ctx;
}

double gpu_energy_stop_joules(GpuEnergyContext *ctx) {
    (void)ctx;
    return 0.0;
}

int gpu_energy_measurement_valid(GpuEnergyContext *ctx) {
    (void)ctx;
    return 0;
}

const char *gpu_energy_method(GpuEnergyContext *ctx) {
    (void)ctx;
    return "unavailable";
}

const char *gpu_energy_error(GpuEnergyContext *ctx) {
    (void)ctx;
    return "NVML disabled at build time";
}

const char *gpu_energy_device_name(GpuEnergyContext *ctx) {
    (void)ctx;
    return "unavailable";
}

void gpu_energy_destroy(GpuEnergyContext *ctx) {
    free(ctx);
}

#endif
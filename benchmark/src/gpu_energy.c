#define _GNU_SOURCE
#include "gpu_energy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdatomic.h>

#ifdef WITH_NVML
#include <nvml.h>
#include <pthread.h>

/*
 * NVML power telemetry strategy:
 *
 * 1. Prefer nvmlDeviceGetTotalEnergyConsumption() when supported.
 * 2. Otherwise use NVML_TOTAL_POWER_SAMPLES through nvmlDeviceGetSamples().
 */

#define SAMPLE_POLL_INTERVAL_MS 10U

struct GpuEnergyContext {
    nvmlDevice_t device;

    int use_total_energy_api;
    unsigned long long start_total_mj;

    int power_samples_supported;

    pthread_t thread;
    int thread_created;
    atomic_int running;

    /* Integration of P(t) using the timestamps supplied by NVML. */
    double sampled_energy_j;

    unsigned long long last_seen_timestamp_us;
    unsigned long long previous_timestamp_us;
    unsigned int previous_power_mw;
    int have_previous_sample;

    unsigned long long sample_count;

    int measurement_valid;
    char method[64];
};

static void sleep_ms(unsigned int ms) {
    struct timespec req;
    req.tv_sec = ms / 1000U;
    req.tv_nsec = (long)(ms % 1000U) * 1000000L;
    nanosleep(&req, NULL);
}

/*
 * Retrieve all total-power samples newer than last_seen.
 */
static nvmlReturn_t fetch_power_samples(
    GpuEnergyContext *ctx,
    unsigned long long last_seen,
    nvmlSample_t **samples_out,
    unsigned int *count_out)
{
    if (!ctx || !samples_out || !count_out)
        return NVML_ERROR_INVALID_ARGUMENT;

    *samples_out = NULL;
    *count_out = 0;

    nvmlValueType_t value_type;
    unsigned int count = 0;

    nvmlReturn_t rc = nvmlDeviceGetSamples(
        ctx->device,
        NVML_TOTAL_POWER_SAMPLES,
        last_seen,
        &value_type,
        &count,
        NULL
    );

    if (rc != NVML_SUCCESS)
        return rc;

    if (value_type != NVML_VALUE_TYPE_UNSIGNED_INT) {
        fprintf(stderr, "NVML: unexpected value type for power samples: %d\n", (int)value_type);
        return NVML_ERROR_UNKNOWN;
    }

    if (count == 0)
        return NVML_ERROR_NOT_FOUND;

    nvmlSample_t *samples = calloc(count, sizeof(*samples));
    if (!samples)
        return NVML_ERROR_MEMORY;

    unsigned int actual_count = count;

    rc = nvmlDeviceGetSamples(
        ctx->device,
        NVML_TOTAL_POWER_SAMPLES,
        last_seen,
        &value_type,
        &actual_count,
        samples
    );

    if (rc != NVML_SUCCESS) {
        free(samples);
        return rc;
    }

    *samples_out = samples;
    *count_out = actual_count;
    return NVML_SUCCESS;
}

/*
 * Integrate power samples with trapezoidal integration:
 *
 * E = sum((P_i + P_(i-1))/2 * dt)
 *
 * Power is supplied in mW, timestamps in microseconds.
 */
static void consume_samples(GpuEnergyContext *ctx, const nvmlSample_t *samples, unsigned int count) {

    if (!ctx || !samples)
        return;

    for (unsigned int i = 0; i < count; ++i) {
        unsigned long long ts = samples[i].timeStamp;
        unsigned int mw = samples[i].sampleValue.uiVal;

        if (ctx->have_previous_sample) {
            if (ts > ctx->previous_timestamp_us) {
                double dt =
                    (double)(ts - ctx->previous_timestamp_us) * 1e-6;

                // A very large gap indicates that samples may have been lost from the driver's ring buffer.
                if (dt <= 1.0) {
                    double p0_w = (double)ctx->previous_power_mw / 1000.0;
                    double p1_w = (double)mw / 1000.0;
                    ctx->sampled_energy_j += ((p0_w + p1_w) * 0.5) * dt;
                } else {
                    ctx->measurement_valid = 0;
                    fprintf(stderr, "NVML: gap of %.6f s between power samples; measurement marked as invalid.\n", dt);
                }
            }
        } else {
            ctx->have_previous_sample = 1;
        }

        ctx->previous_timestamp_us = ts;
        ctx->previous_power_mw = mw;
        ctx->last_seen_timestamp_us = ts;
        ctx->sample_count++;
    }
}

static void drain_power_samples(GpuEnergyContext *ctx) {

    nvmlSample_t *samples = NULL;
    unsigned int count = 0;

    nvmlReturn_t rc = fetch_power_samples(
        ctx,
        ctx->last_seen_timestamp_us,
        &samples,
        &count
    );

    if (rc == NVML_SUCCESS) {
        consume_samples(ctx, samples, count);
        free(samples);
    } else if (rc != NVML_ERROR_NOT_FOUND) {
        fprintf(stderr, "NVML: failed to get power samples: %s\n", nvmlErrorString(rc));
        ctx->measurement_valid = 0;
    }
}

static void *sample_thread_fn(void *arg) {
    GpuEnergyContext *ctx = (GpuEnergyContext *)arg;

    while (atomic_load_explicit(&ctx->running, memory_order_relaxed)) {
        drain_power_samples(ctx);
        sleep_ms(SAMPLE_POLL_INTERVAL_MS);
    }

    // Final drain after the measured batch has ended.
    drain_power_samples(ctx);

    return NULL;
}

int gpu_energy_available(void) {
    return 1;
}

GpuEnergyContext *gpu_energy_init(int device_index) {

    nvmlReturn_t rc = nvmlInit();
    if (rc != NVML_SUCCESS) {
        fprintf(stderr, "NVML: nvmlInit failed: %s\n", nvmlErrorString(rc));
        return NULL;
    }

    GpuEnergyContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        nvmlShutdown();
        return NULL;
    }

    rc = nvmlDeviceGetHandleByIndex(
        (unsigned int)device_index,
        &ctx->device
    );

    if (rc != NVML_SUCCESS) {
        fprintf(stderr, "NVML: GPU %d unavailable: %s\n", device_index, nvmlErrorString(rc));
        free(ctx);
        nvmlShutdown();
        return NULL;
    }

    /*
     * Preferred method: cumulative energy counter.
     */
    unsigned long long test_mj = 0;
    nvmlReturn_t energy_rc = nvmlDeviceGetTotalEnergyConsumption(ctx->device, &test_mj);

    if (energy_rc == NVML_SUCCESS) {
        ctx->use_total_energy_api = 1;
        ctx->power_samples_supported = 0;

        snprintf(ctx->method, sizeof(ctx->method), "NVML_total_energy");

        fprintf(stderr, "NVML: cumulative energy available (mJ).\n");

        ctx->measurement_valid = 1;
        return ctx;
    }

    /*
     * Fallback: driver-maintained total-power samples (GTX 1050 TI)
     */
    nvmlValueType_t value_type;
    unsigned int count = 0;

    nvmlReturn_t samples_rc = nvmlDeviceGetSamples(
        ctx->device,
        NVML_TOTAL_POWER_SAMPLES,
        0,
        &value_type,
        &count,
        NULL
    );

    if (samples_rc == NVML_SUCCESS &&
        value_type == NVML_VALUE_TYPE_UNSIGNED_INT) {

        ctx->use_total_energy_api = 0;
        ctx->power_samples_supported = 1;
        ctx->measurement_valid = 1;

        snprintf(ctx->method, sizeof(ctx->method), "NVML_power_samples");

        fprintf(stderr, "NVML: cumulative energy not supported: %s\n", nvmlErrorString(energy_rc));
        fprintf(stderr, "NVML: using NVML_TOTAL_POWER_SAMPLES " "(samples kept by the driver).\n");

        return ctx;
    }

    fprintf(stderr, "NVML: cumulative energy unavailable: %s\n", nvmlErrorString(energy_rc));
    fprintf(stderr, "NVML: power samples unavailable: %s\n", nvmlErrorString(samples_rc));

    free(ctx);
    nvmlShutdown();
    return NULL;
}

void gpu_energy_start(GpuEnergyContext *ctx) {

    if (!ctx)
        return;

    ctx->measurement_valid = 1;
    ctx->sampled_energy_j = 0.0;
    ctx->sample_count = 0;
    ctx->thread_created = 0;
    ctx->have_previous_sample = 0;
    ctx->last_seen_timestamp_us = 0;
    ctx->previous_timestamp_us = 0;
    ctx->previous_power_mw = 0;

    if (ctx->use_total_energy_api) {
        nvmlReturn_t rc =
            nvmlDeviceGetTotalEnergyConsumption(
                ctx->device,
                &ctx->start_total_mj
            );

        if (rc != NVML_SUCCESS) {
            fprintf(stderr, "NVML: initial energy read failed: %s\n", nvmlErrorString(rc));
            ctx->measurement_valid = 0;
        }

        return;
    }

    if (!ctx->power_samples_supported) {
        ctx->measurement_valid = 0;
        return;
    }

    nvmlSample_t *samples = NULL;
    unsigned int count = 0;

    nvmlReturn_t rc = fetch_power_samples(ctx, 0, &samples, &count);

    if (rc == NVML_SUCCESS && count > 0) {
        const nvmlSample_t *last = &samples[count - 1];

        ctx->last_seen_timestamp_us = last->timeStamp;
        ctx->previous_timestamp_us = last->timeStamp;
        ctx->previous_power_mw = last->sampleValue.uiVal;
        ctx->have_previous_sample = 1;

        free(samples);
    } else if (rc != NVML_ERROR_NOT_FOUND) {
        fprintf(stderr, "NVML: failed to establish power samples baseline: %s\n", nvmlErrorString(rc));
        free(samples);
        ctx->measurement_valid = 0;
        return;
    }

    atomic_store_explicit(&ctx->running, 1, memory_order_relaxed);

    int prc = pthread_create(
        &ctx->thread,
        NULL,
        sample_thread_fn,
        ctx
    );

    if (prc != 0) {
        fprintf(stderr, "NVML: pthread_create failed: %s\n", strerror(prc));
        atomic_store_explicit(&ctx->running, 0, memory_order_relaxed);
        ctx->measurement_valid = 0;
        return;
    }

    ctx->thread_created = 1;
}

double gpu_energy_stop_joules(GpuEnergyContext *ctx) {

    if (!ctx)
        return NAN;

    if (ctx->use_total_energy_api) {
        unsigned long long end_mj = 0;

        nvmlReturn_t rc =
            nvmlDeviceGetTotalEnergyConsumption(
                ctx->device,
                &end_mj
            );

        if (rc != NVML_SUCCESS) {
            fprintf(stderr, "NVML: final energy read failed: %s\n", nvmlErrorString(rc));
            ctx->measurement_valid = 0;
            return NAN;
        }

        if (end_mj < ctx->start_total_mj) {
            fprintf(stderr, "NVML: energy counter went backwards.\n");
            ctx->measurement_valid = 0;
            return NAN;
        }

        return (double)(end_mj - ctx->start_total_mj) / 1000.0;
    }

    if (!ctx->power_samples_supported) {
        ctx->measurement_valid = 0;
        return NAN;
    }

    atomic_store_explicit(&ctx->running, 0, memory_order_relaxed);

    if (ctx->thread_created) {
        pthread_join(ctx->thread, NULL);
        ctx->thread_created = 0;
    }

    /*
     * We need at least two distinct driver samples to integrate P(t).
     */
    if (ctx->sample_count < 2) {
        fprintf(stderr, "NVML: insufficient power samples: %llu\n", ctx->sample_count);
        ctx->measurement_valid = 0;
        return NAN;
    }

    if (!ctx->measurement_valid)
        return NAN;

    return ctx->sampled_energy_j;
}

int gpu_energy_measurement_valid(const GpuEnergyContext *ctx) {
    return ctx && ctx->measurement_valid;
}

const char *gpu_energy_method(const GpuEnergyContext *ctx) {
    return ctx ? ctx->method : "unavailable";
}

void gpu_energy_shutdown(GpuEnergyContext *ctx) {

    if (!ctx)
        return;

    free(ctx);
    nvmlShutdown();
}

#else

struct GpuEnergyContext {
    int unused;
};

int gpu_energy_available(void) {
    return 0;
}

GpuEnergyContext *gpu_energy_init(int device_index) {
    (void)device_index;
    return NULL;
}

void gpu_energy_start(GpuEnergyContext *ctx) {
    (void)ctx;
}

double gpu_energy_stop_joules(GpuEnergyContext *ctx) {
    (void)ctx;
    return NAN;
}

int gpu_energy_measurement_valid(const GpuEnergyContext *ctx) {
    (void)ctx;
    return 0;
}

const char *gpu_energy_method(const GpuEnergyContext *ctx) {
    (void)ctx;
    return "unavailable";
}

void gpu_energy_shutdown(GpuEnergyContext *ctx) {
    (void)ctx;
}

#endif
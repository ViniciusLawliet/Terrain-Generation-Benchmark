#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nvml.h>

static void print_rc(const char *label, nvmlReturn_t rc) {
    printf("%s: %s (%d)\n", label, nvmlErrorString(rc), (int)rc);
}

static int probe_power_sample_buffer(nvmlDevice_t dev) {
    nvmlValueType_t value_type = NVML_VALUE_TYPE_UNSIGNED_INT;
    unsigned int count = 0;

    nvmlReturn_t rc = nvmlDeviceGetSamples(
        dev, NVML_TOTAL_POWER_SAMPLES, 0, &value_type, &count, NULL);

    print_rc("nvmlDeviceGetSamples(size)", rc);
    printf("  sample_count_available: %u\n", count);

    if (rc != NVML_SUCCESS || count == 0)
        return 0;

    nvmlSample_t *samples = calloc(count, sizeof(*samples));
    if (!samples) {
        printf("  allocation: failed\n");
        return 0;
    }

    value_type = NVML_VALUE_TYPE_UNSIGNED_INT;
    unsigned int capacity = count;

    rc = nvmlDeviceGetSamples(
        dev, NVML_TOTAL_POWER_SAMPLES, 0, &value_type, &capacity, samples);

    print_rc("nvmlDeviceGetSamples(data)", rc);

    int ok = 0;
    if (rc == NVML_SUCCESS && capacity > 0) {
        printf("  returned_samples: %u\n", capacity);
        printf("  first: timestamp_us=%llu power=%.3f W\n",
               samples[0].timeStamp,
               samples[0].sampleValue.uiVal / 1000.0);
        printf("  last:  timestamp_us=%llu power=%.3f W\n",
               samples[capacity - 1].timeStamp,
               samples[capacity - 1].sampleValue.uiVal / 1000.0);
        ok = 1;
    }

    free(samples);
    return ok;
}

int main(int argc, char **argv) {
    unsigned int index = argc > 1
        ? (unsigned int)strtoul(argv[1], NULL, 10)
        : 0;

    nvmlReturn_t rc = nvmlInit_v2();
    if (rc != NVML_SUCCESS) {
        print_rc("nvmlInit_v2", rc);
        return 1;
    }

    nvmlDevice_t dev;
    rc = nvmlDeviceGetHandleByIndex(index, &dev);
    if (rc != NVML_SUCCESS) {
        print_rc("nvmlDeviceGetHandleByIndex", rc);
        nvmlShutdown();
        return 1;
    }

    char name[NVML_DEVICE_NAME_V2_BUFFER_SIZE] = {0};
    rc = nvmlDeviceGetName(dev, name, sizeof(name));
    if (rc != NVML_SUCCESS)
        snprintf(name, sizeof(name), "unknown");

    unsigned long long energy_mj = 0;
    nvmlReturn_t energy_rc = nvmlDeviceGetTotalEnergyConsumption(dev, &energy_mj);

    int samples_ok = probe_power_sample_buffer(dev);

    printf("GPU: %s\n", name);
    print_rc("nvmlDeviceGetTotalEnergyConsumption", energy_rc);
    if (energy_rc == NVML_SUCCESS)
        printf("  total_energy: %llu mJ\n", energy_mj);

    if (energy_rc == NVML_SUCCESS)
        printf("selected_method: nvml_total_energy_consumption\n");
    else if (samples_ok)
        printf("selected_method: nvml_power_samples_buffer\n");
    else
        printf("selected_method: unavailable\n");

    nvmlShutdown();
    return 0;
}
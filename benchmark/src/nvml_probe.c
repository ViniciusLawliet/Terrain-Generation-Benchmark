#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nvml.h>

#ifndef NVML_FI_DEV_POWER_INSTANT
#define NVML_FI_DEV_POWER_INSTANT 186
#endif

static void print_rc(const char *label, nvmlReturn_t rc) {
    printf("%s: %s (%d)\n", label, nvmlErrorString(rc), (int)rc);
}


static void probe_power_sample_buffer(nvmlDevice_t dev) {
    nvmlValueType_t value_type = NVML_VALUE_TYPE_UNSIGNED_INT;
    unsigned int count = 0;
    nvmlReturn_t rc = nvmlDeviceGetSamples(
        dev, NVML_TOTAL_POWER_SAMPLES, 0, &value_type, &count, NULL);

    print_rc("nvmlDeviceGetSamples(size)", rc);
    printf("  sample_count_available: %u\n", count);
    if (rc != NVML_SUCCESS || count == 0)
        return;

    nvmlSample_t *samples = (nvmlSample_t *)calloc(count, sizeof(*samples));
    if (!samples) {
        printf("  allocation: failed\n");
        return;
    }

    value_type = NVML_VALUE_TYPE_UNSIGNED_INT;
    unsigned int capacity = count;
    rc = nvmlDeviceGetSamples(
        dev, NVML_TOTAL_POWER_SAMPLES, 0, &value_type, &capacity, samples);

    print_rc("nvmlDeviceGetSamples(data)", rc);
    if (rc == NVML_SUCCESS) {
        printf("  returned_samples: %u\n", capacity);
        if (capacity > 0) {
            unsigned int first = 0;
            unsigned int last = capacity - 1;
            printf("  first: timestamp_us=%llu power=%.3f W\n",
                   samples[first].timeStamp,
                   samples[first].sampleValue.uiVal / 1000.0);
            printf("  last:  timestamp_us=%llu power=%.3f W\n",
                   samples[last].timeStamp,
                   samples[last].sampleValue.uiVal / 1000.0);
        }
    }

    free(samples);
}

int main(int argc, char **argv) {
    unsigned int index = argc > 1 ? (unsigned int)strtoul(argv[1], NULL, 10) : 0;

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

    unsigned int power_mw = 0;
    nvmlReturn_t power_rc = nvmlDeviceGetPowerUsage(dev, &power_mw);

    nvmlFieldValue_t field;
    memset(&field, 0, sizeof(field));
    field.fieldId = NVML_FI_DEV_POWER_INSTANT;
    field.scopeId = 0;
    nvmlReturn_t field_rc = nvmlDeviceGetFieldValues(dev, 1, &field);

    unsigned long long energy_mj = 0;
    nvmlReturn_t energy_rc = nvmlDeviceGetTotalEnergyConsumption(dev, &energy_mj);

    printf("GPU: %s\n", name);
    print_rc("nvmlDeviceGetPowerUsage", power_rc);
    if (power_rc == NVML_SUCCESS)
        printf("  power: %.3f W\n", power_mw / 1000.0);

    print_rc("nvmlDeviceGetFieldValues", field_rc);
    if (field_rc == NVML_SUCCESS) {
        print_rc("  NVML_FI_DEV_POWER_INSTANT", field.nvmlReturn);
        printf("  fieldId: %u\n", field.fieldId);
        printf("  valueType: %d\n", (int)field.valueType);
        if (field.nvmlReturn == NVML_SUCCESS && field.valueType == NVML_VALUE_TYPE_UNSIGNED_INT)
            printf("  power_instant: %.3f W\n", field.value.uiVal / 1000.0);
    }

    print_rc("nvmlDeviceGetTotalEnergyConsumption", energy_rc);
    if (energy_rc == NVML_SUCCESS)
        printf("  total energy: %llu mJ\n", energy_mj);

    nvmlValueType_t sample_type = NVML_VALUE_TYPE_UNSIGNED_INT;
    unsigned int sample_count_available = 0;
    nvmlReturn_t sample_probe_rc = nvmlDeviceGetSamples(
        dev, NVML_TOTAL_POWER_SAMPLES, 0, &sample_type,
        &sample_count_available, NULL);
    probe_power_sample_buffer(dev);

    if (energy_rc == NVML_SUCCESS)
        printf("selected_method: nvml_total_energy_consumption\n");
    else if (power_rc == NVML_SUCCESS)
        printf("selected_method: nvml_power_usage_sampling\n");
    else if (field_rc == NVML_SUCCESS && field.nvmlReturn == NVML_SUCCESS &&
             field.valueType == NVML_VALUE_TYPE_UNSIGNED_INT)
        printf("selected_method: nvml_field_power_instant_sampling\n");
    else if ((sample_probe_rc == NVML_SUCCESS || sample_probe_rc == NVML_ERROR_NOT_FOUND) &&
             sample_count_available > 0)
        printf("selected_method: nvml_power_samples_buffer\n");
    else
        printf("selected_method: unavailable\n");

    nvmlShutdown();
    return 0;
}

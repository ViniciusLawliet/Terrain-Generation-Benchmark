#ifndef BENCH_GPU_ENERGY_H
#define BENCH_GPU_ENERGY_H

typedef struct GpuEnergyContext GpuEnergyContext;

GpuEnergyContext *gpu_energy_init(int device_index);
void gpu_energy_start(GpuEnergyContext *ctx);
double gpu_energy_stop_joules(GpuEnergyContext *ctx);
int gpu_energy_measurement_valid(GpuEnergyContext *ctx);
const char *gpu_energy_method(GpuEnergyContext *ctx);
const char *gpu_energy_error(GpuEnergyContext *ctx);
const char *gpu_energy_device_name(GpuEnergyContext *ctx);
void gpu_energy_destroy(GpuEnergyContext *ctx);

#endif
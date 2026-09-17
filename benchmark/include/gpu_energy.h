#ifndef GPU_ENERGY_H
#define GPU_ENERGY_H

typedef struct GpuEnergyContext GpuEnergyContext;

int gpu_energy_available(void);
GpuEnergyContext *gpu_energy_init(int device_index);
void gpu_energy_start(GpuEnergyContext *ctx);
double gpu_energy_stop_joules(GpuEnergyContext *ctx);
int gpu_energy_measurement_valid(const GpuEnergyContext *ctx);
const char *gpu_energy_method(const GpuEnergyContext *ctx);
void gpu_energy_shutdown(GpuEnergyContext *ctx);

#endif
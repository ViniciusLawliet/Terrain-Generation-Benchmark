# Benchmark Results

This page summarizes the performance and energy measurements for the procedural terrain generator. It covers execution time, (...), energy consumption, energy reduction relative to the single-threaded CPU baseline, and Energy–Delay Product (EDP). These metrics are evaluated under default operating conditions and with static CPU and GPU (???) frequency tuning to investigate potential energy savings and performance–energy trade-offs.

## Experimental setup

- **CPU:** Intel Core i5-2400 (4 cores / 4 threads), 8 GB DDR3 RAM.
- **GPU:** NVIDIA GeForce GTX 1050 Ti with 4 GB GDDR5.
- **Operating system:** Ubuntu 22.04.5 LTS.
- **Implementations:** sequential baseline (the OpenMP executable run with one thread), OpenMP with 2 threads, OpenMP with 4 threads, and CUDA.
- **Workloads:** cubic scalar fields with dimensions `256³`, `512³`, `768³`, and `1024³`.
- **Repetitions:** 10 successful measured runs per configuration, following one warm-up run.
- **Random seed:** `1234`.
- **Measured execution:** terrain field generation and Marching Cubes. OBJ export was disabled during the benchmark runs.

## Commands Used for the Experiments

The following commands were used to collect the benchmark measurements:

```bash
mkdir -p results/sequential results/openmp_2t results/openmp_4t results/cuda

for N in 256 512 768 1024; do
    ./benchmark/bench \
        -b ./openmp/terrain_gen_openmp -n 10 \
        -o "./results/sequential/n${N}" \
        -- -x "$N" -y "$N" -z "$N" -t 1 -s 1234 || break
done

for N in 256 512 768 1024; do
    ./benchmark/bench \
        -b ./openmp/terrain_gen_openmp -n 10 \
        -o "./results/openmp_2t/n${N}" \
        -- -x "$N" -y "$N" -z "$N" -t 2 -s 1234 || break
done

for N in 256 512 768 1024; do
    ./benchmark/bench \
        -b ./openmp/terrain_gen_openmp -n 10 \
        -o "./results/openmp_4t/n${N}" \
        -- -x "$N" -y "$N" -z "$N" -t 4 -s 1234 || break
done

for N in 256 512 768 1024; do
    ./benchmark/bench \
        -b ./cuda/terrain_gen_cuda -n 10 -g \
        -o "./results/cuda/n${N}" \
        -- -x "$N" -y "$N" -z "$N" -s 1234 || break
done
```

### Measurement scope

CPU energy is reported as CPU package energy measured through Linux RAPL/powercap. For CUDA runs, the reported energy is the sum of CPU package energy and GPU energy estimated by integrating NVML power samples over the measurement interval. The recorded CUDA method is `nvml_power_samples_buffer`.

**The energy scope is not identical across all variants.** The sequential and OpenMP runs report CPU package energy only; they do not include the energy consumed by the GPU while idle. CUDA runs report CPU package plus GPU energy.

## 1. Execution time and scaling with grid size

...

## 2. Measured energy and scaling with grid size

...

## 3. Speedup and parallel efficiency

...

## 5. Energy reduction and Energy–Delay Product

...

## Summary at the largest tested grid (`1024³`)

| Implementation | Mean time (s) | Speedup (×) | OpenMP efficiency | Mean measured energy (J) | Reported energy reduction vs. sequential | Mean EDP (J·s) |
|---|---:|---:|---:|---:|---:|---:|
table?

## Main findings (no fixed frequency)

...

## Methodology and metric definitions

- **Speedup:** `T_sequential / T_variant`, calculated from mean execution times for the same grid size (only for OpenMP ???).
- **Reported energy reduction:** `(E_sequential − E_variant) / E_sequential × 100%`, calculated from mean reported measured energy.
- **EDP:** the benchmark-reported mean of per-run energy–delay products, in `J·s`.

---
# Static frequency experiments:

...

## Main findings (whit fixed frequency)

cmds used in adjust frequency...

# General Main findings (both?)

(...)
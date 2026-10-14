# Usage Guide

This guide explains how to install the required tools, enable CPU energy readings through Linux RAPL, build the project, run the benchmark, and export terrain meshes.

The instructions were tested on **Ubuntu 22.04.5 LTS (Jammy Jellyfish)** with an Intel Core i5-2400 CPU and an NVIDIA GeForce GTX 1050 Ti.

## Contents

- [1. Requirements](#1-requirements)
- [2. Enable CPU energy readings with RAPL](#2-enable-cpu-energy-readings-with-rapl)
- [3. Build the project](#3-build-the-project)
- [4. Run the benchmark](#4-run-the-benchmark)
- [5. Export a terrain mesh without the benchmark](#5-export-a-terrain-mesh-without-the-benchmark)
- [6. Terrain generator options](#6-terrain-generator-options)
- [7. Troubleshooting](#7-troubleshooting)

## 1. Requirements

### CPU / OpenMP

- **GCC** with C and OpenMP support.
- **GNU Make**.
- A Linux kernel and Intel CPU exposing the RAPL energy counters through the `powercap` interface for CPU energy measurements.
- Read access to the RAPL `energy_uj` counter files.

RAPL/powercap is a Linux kernel interface; it is not a separate library that needs to be installed. Availability depends on the CPU, kernel, firmware, and system configuration.

### GPU / CUDA

- An installed **NVIDIA driver**, providing `nvidia-smi` and the NVML runtime library (usually `libnvidia-ml.so.1`).
- An NVIDIA GPU supported by the installed driver.
- The **CUDA Toolkit**, including `nvcc`, to compile the CUDA executable.
- The NVML development header `nvml.h` to compile GPU energy monitoring into the benchmark. It may be included with the available CUDA/NVIDIA development packages; package names and header locations vary by distribution.

The current `cuda/Makefile` sets `CUDA_ARCH = -arch=sm_61`, which matches the NVIDIA GeForce GTX 1050 Ti used for the experiments. When compiling for a different GPU architecture, review and update this setting in `cuda/Makefile` as needed.

### Check the installation

```bash
gcc --version
make --version
nvidia-smi
nvcc --version
```

If `nvidia-smi` or `nvcc` is not found, check the NVIDIA driver or CUDA Toolkit installation, respectively. The NVIDIA runtime library is required at run time for NVML monitoring; the header is needed when compiling the benchmark with NVML support.

## 2. Enable CPU energy readings with RAPL

The benchmark reads energy counters from paths under `/sys/class/powercap`. On the tested machine, the current user needed read access to the RAPL `energy_uj` files through a `powercap` group.

### Create the group and add your user

The first command creates the group only if it does not already exist:

```bash
getent group powercap || sudo groupadd powercap
sudo usermod -aG powercap "$USER"
```

### Add a udev rule

Open a rule file:

```bash
sudo nano /etc/udev/rules.d/70-intel-rapl.rules
```

Add this rule as **one line**:

```udev
ACTION=="add", SUBSYSTEM=="powercap", KERNEL=="intel-rapl:*", RUN+="/usr/bin/chgrp powercap /sys/%p/energy_uj", RUN+="/usr/bin/chmod g+r /sys/%p/energy_uj"
```

Save the file, then reload the rules and re-trigger the `powercap` devices:

```bash
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=powercap
```

Refresh group membership for the current shell:

```bash
newgrp powercap
```

Alternatively, log out and log back in so the new group membership is applied to your login session. On the test machine, `newgrp powercap` was used in each terminal session used to run measurements.

### Verify RAPL access

List the available powercap devices:

```bash
ls /sys/class/powercap/
```

Check whether the expected Intel RAPL energy counter exists and is readable by your user:

```bash
ls -l /sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj
cat /sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj
```

The benchmark's current RAPL reader expects the Intel domain layout shown above. Some systems expose different paths or domains; those systems may require adapting the reader in `benchmark/src/rapl.c`.

> **Permissions note:** The udev rule changes the group and read permission for matching Intel RAPL energy counter files. Review local security policies before applying it on a shared machine.

## 3. Build the project

From the repository root, run:

```bash
make
```

The top-level Makefile builds the shared code, OpenMP executable, benchmark harness, and CUDA executable. After a successful full build, these executables should exist:

```text
./openmp/terrain_gen_openmp
./cuda/terrain_gen_cuda
./benchmark/bench
```

To remove generated object files and executables before rebuilding:

```bash
make clean
make
```

A full build requires the GCC/OpenMP toolchain, GNU Make, CUDA Toolkit, and NVML development headers and library. If `nvml.h` is installed in a non-default location, you can pass its include directory to Make, for example:

```bash
make NVML_INC="-I/path/to/nvml/include"
```

If you need to build the benchmark without NVML support, use `WITH_NVML=0`. For a CPU-only build that omits the CUDA target, build the CPU dependencies and executable directly:

```bash
make common openmp
make -C benchmark WITH_NVML=0
```

With `WITH_NVML=0`, the benchmark cannot collect GPU energy measurements. Building the CUDA terrain generator itself still requires `nvcc` and a compatible NVIDIA GPU architecture setting.

## 4. Run the benchmark

The benchmark harness runs a selected executable repeatedly, measures execution time, reads CPU energy through RAPL, and can optionally collect GPU energy through NVML. It writes two CSV files for each output prefix:

- `<prefix>_runs.csv` — measurements from individual executions.
- `<prefix>_summary.csv` — aggregate statistics, including means, standard deviations, and energy/EDP summaries.

The output option `-o` specifies a **file prefix**, not a directory that the program creates. Ensure the parent directory exists before starting the benchmark.

### Representative commands

The following commands use a cubic terrain grid of **512 × 512 × 512** (`X × Y × Z`) and perform 10 measured runs per configuration. Results are stored in separate directories for each implementation, following the structure of the existing results/dataset.

```bash
mkdir -p results/sequential results/openmp_4t results/cuda
```

**Sequential CPU baseline** — the OpenMP executable is restricted to one thread:

```bash
./benchmark/bench -b ./openmp/terrain_gen_openmp -n 10 -o ./results/sequential/n512 -- -x 512 -y 512 -z 512 -t 1
```

**OpenMP with four CPU threads:**

```bash
./benchmark/bench -b ./openmp/terrain_gen_openmp -n 10 -o ./results/openmp_4t/n512 -- -x 512 -y 512 -z 512 -t 4
```

**CUDA on the GPU**, with NVML energy monitoring enabled:

```bash
./benchmark/bench -b ./cuda/terrain_gen_cuda -n 10 -g -o ./results/cuda/n512 -- -x 512 -y 512 -z 512
```

The resulting directory structure is:

```text
results/
├── sequential/
│   ├── n512_runs.csv
│   └── n512_summary.csv
├── openmp_4t/
│   ├── n512_runs.csv
│   └── n512_summary.csv
└── cuda/
    ├── n512_runs.csv
    └── n512_summary.csv
```

The harness performs one unmeasured warm-up execution by default, followed by the 10 measured runs. The commands above do not pass the generator's OBJ export option (-o), so writing the generated mesh to disk is excluded from the benchmark measurements.

### Benchmark options

Options before `--` belong to the benchmark harness. Arguments after `--` are passed to the selected terrain executable.

| Option | Description |
| --- | --- |
| `-b <path>` | Target executable to run. |
| `-n <count>` | Number of measured executions. Must be greater than zero. |
| `-o <prefix>` | Prefix used to create the `_runs.csv` and `_summary.csv` files. Its parent directory must already exist. |
| `-g` | Enable GPU energy measurement through NVML. Use this for CUDA benchmark runs. |
| `--gpu-index <n>` | GPU index to monitor; default: `0`. |
| `--warmup <n>` | Number of unmeasured warm-up executions; default: `1`. |
| `--energy-batch <n>` | Number of measured executions per energy-measurement batch; default: `1`. |
| `-h`, `--help` | Display the benchmark's help message. |

For example, to use two warm-up executions, add `--warmup 2` before the `--` separator. The default `--energy-batch 1` measures energy separately for each execution. Larger batches measure energy over a group of executions and report per-execution averages for the energy-related metrics.

### GPU energy measurement methods

When `-g` is enabled, the harness first attempts to read NVML's total energy consumption counter. If that counter is not supported, it attempts to use NVML's power-sample buffer and estimates energy by integrating the samples over the measurement interval. The selected method is printed by the benchmark.

If neither method is available, the CUDA program can still run, but GPU energy will be unavailable. The presence of `nvidia-smi` alone does not guarantee that the installed GPU/driver exposes a supported energy measurement method.

## 5. Export a terrain mesh without the benchmark

You do not need the benchmark harness when you only want to generate a terrain mesh. Run the desired executable directly and pass `-o <file.obj>` to export the resulting mesh.

**CPU/OpenMP:**

```bash
./openmp/terrain_gen_openmp \
  -x 256 -y 32 -z 256 -t 4 -s 1234 \
  -o terrain-openmp.obj
```

**CUDA:**

```bash
./cuda/terrain_gen_cuda \
  -x 256 -y 32 -z 256 -s 1234 \
  -o terrain-cuda.obj
```

Open the generated Wavefront OBJ files in Blender, MeshLab, or another compatible 3D application. If `-o` is omitted, no OBJ file is written.

## 6. Terrain generator options

These options are passed to `terrain_gen_openmp` or `terrain_gen_cuda` directly, or after `--` when invoking the benchmark harness.

| Option | Meaning |
| --- | --- |
| `-s <seed>` | Perlin Noise seed; default: `1234`. |
| `-x <Nx>` | Scalar-field width along the X axis; default: `256`. |
| `-y <Ny>` | Scalar-field height / vertical extent along the Y axis; default: `32`. |
| `-z <Nz>` | Scalar-field depth along the Z axis; default: `256`. |
| `-i <isovalue>` | Marching Cubes isovalue; default: `0.0`. |
| `-f <frequency>` | Horizontal noise frequency; default: `6.0 / max(Nx, Nz)`. |
| `-a <amplitude>` | Vertical terrain amplitude in cells; default: `0.4 * Ny`. |
| `-t <threads>` | OpenMP thread count for the CPU implementation. In the CUDA executable, this controls the thread count used for OBJ export. |
| `-o <file.obj>` | Export the generated terrain to a Wavefront OBJ file. If omitted, OBJ export is disabled. |
| `-h` | Display the terrain executable's help message. |

For available options, run:

```bash
./openmp/terrain_gen_openmp -h
./cuda/terrain_gen_cuda -h
./benchmark/bench -h
```

## 7. Troubleshooting

- **RAPL is unavailable or CPU energy is `N/A`:** confirm that the CPU/kernel exposes Intel RAPL, inspect `/sys/class/powercap/`, verify that the expected `energy_uj` file is readable by your user, and confirm that the path matches the reader in `benchmark/src/rapl.c`.
- **`nvcc` is not found:** install the CUDA Toolkit and ensure its `bin` directory is on `PATH`.
- **`nvml.h` is missing during compilation:** install the NVML development headers or pass their directory with `NVML_INC="-I/path/to/nvml/include"` when running `make`.
- **NVML library cannot be linked or loaded:** verify the NVIDIA driver installation and that `libnvidia-ml.so.1` is visible to the dynamic linker. GPU measurement can also be disabled at benchmark build time with `WITH_NVML=0`.
- **GPU energy is unavailable at run time:** check `nvidia-smi`, then inspect the benchmark's NVML warning. The GPU/driver combination must support either the total-energy counter or NVML power samples.
- **The executable cannot be created for a different GPU:** check `CUDA_ARCH` in `cuda/Makefile`; the current value targets `sm_61`.
- **CSV files cannot be created:** ensure that the directory containing the `-o` prefix exists before starting the benchmark.

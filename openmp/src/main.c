#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <omp.h>

#include "perlin.h"
#include "terrain_openmp.h"
#include "obj_export.h"

typedef struct {
    uint64_t seed;
    int Nx;
    int Ny;
    int Nz;
    float isovalue;
    float frequency;
    int frequency_set;
    float amplitude;
    int amplitude_set;
    int threads;
    int output_set;
    char output[1024];
} Params;

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -s <seed>         Perlin noise seed (default: 1234)\n"
        "  -x <Nx>           Scalar field width (X axis) (default: 256)\n"
        "  -y <Ny>           Scalar field height (Y axis, vertical) (default: 32)\n"
        "  -z <Nz>           Scalar field depth (Z axis) (default: 256)\n"
        "  -i <isovalue>     Marching Cubes isovalue (default: 0.0)\n"
        "  -f <frequency>    Horizontal noise frequency (default: 6.0/max(Nx, Nz))\n"
        "  -a <amplitude>    Vertical terrain amplitude, in cells (default: 0.4*Ny)\n"
        "  -t <threads>      Number of OpenMP threads (default: automatic)\n"
        "  -o <file.obj>     Output file. If omitted, the OBJ is not exported\n"
        "                    and OBJ export time is excluded from the benchmark.\n"
        "  -h                Show this help message\n"
        "\n"
        "Example:\n"
        "  %s -x 256 -z 256 -y 128 -o terrain.obj\n",
        prog, prog);
}

static Params parse_args(int argc, char **argv) {
    Params p;
    p.seed = 1234;
    p.Nx = 256;
    p.Ny = 32;
    p.Nz = 256;
    p.isovalue = 0.0f;
    p.frequency = 0.0f;
    p.frequency_set = 0;
    p.amplitude = 0.0f;
    p.amplitude_set = 0;
    p.threads = 0;
    p.output_set = 0;
    p.output[0] = '\0';

    int opt;
    while ((opt = getopt(argc, argv, "s:x:y:z:i:f:a:t:o:h")) != -1) {
        switch (opt) {
            case 's': p.seed = strtoull(optarg, NULL, 10); break;
            case 'x': p.Nx = atoi(optarg); break;
            case 'y': p.Ny = atoi(optarg); break;
            case 'z': p.Nz = atoi(optarg); break;
            case 'i': p.isovalue = strtof(optarg, NULL); break;
            case 'f': p.frequency = strtof(optarg, NULL); p.frequency_set = 1; break;
            case 'a': p.amplitude = strtof(optarg, NULL); p.amplitude_set = 1; break;
            case 't': p.threads = atoi(optarg); break;
            case 'o':
                p.output_set = 1;
                strncpy(p.output, optarg, sizeof(p.output) - 1);
                p.output[sizeof(p.output) - 1] = '\0';
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                exit(opt == 'h' ? 0 : 1);
        }
    }

    if (p.Nx < 2 || p.Ny < 2 || p.Nz < 2) {
        fprintf(stderr, "Error: Nx, Ny and Nz must be >= 2\n");
        exit(1);
    }
    if (!p.frequency_set) {
        int maxhoriz = p.Nx > p.Nz ? p.Nx : p.Nz;
        p.frequency = 6.0f / (float)maxhoriz;
    }
    if (!p.amplitude_set) {
        p.amplitude = 0.4f * (float)p.Ny;
    }

    return p;
}

int main(int argc, char **argv) {

    Params params = parse_args(argc, argv);
    int nthreads = params.threads > 0 ? params.threads : omp_get_max_threads();

    omp_set_num_threads(nthreads);

    int Nx = params.Nx, Ny = params.Ny, Nz = params.Nz;

    printf("seed: %llu\n", (unsigned long long)params.seed);
    printf("grid: %d x %d x %d (x,y,z)\n", Nx, Ny, Nz);
    printf("isovalue: %f\n", params.isovalue);
    printf("frequency: %f\n", params.frequency);
    printf("amplitude: %f\n", params.amplitude);
    printf("threads: %d\n", nthreads);
    printf("output: %s\n", params.output_set ? params.output : "(disabled)");

    size_t heightmap_elems = (size_t)Nx * (size_t)Nz;

    float *heightmap = (float *)malloc(heightmap_elems * sizeof(float));

    if (!heightmap) {
        fprintf(stderr, "Error: memory allocation failed (heightmap=%zu, field=%zu elements)\n", heightmap_elems);
        free(heightmap);
        return 1;
    }

    PerlinState perlin_state;
    perlin_init(&perlin_state, params.seed);

    double t0 = omp_get_wtime(); // Perlin Noise (Heightmap)

    float base_height = 0.5f * (float)Ny;

    #pragma omp parallel for collapse(2) schedule(static)
    for (int k = 0; k < Nz; k++) {
        for (int i = 0; i < Nx; i++) {
            double x = (double)i * params.frequency;
            double z = (double)k * params.frequency;
            double h = perlin_noise3(&perlin_state, x, 0.0, z);
            heightmap[(size_t)k * Nx + i] = base_height + (float)h * params.amplitude;
        }
    }

    double t1 = omp_get_wtime(); // Marching Cubes

    TriMesh mesh = marching_cubes_run(heightmap, Nx, Ny, Nz, params.isovalue, nthreads);

    double t2 = omp_get_wtime(); // Export Mesh OBJ

    if (params.output_set) {
        int rc = export_obj(params.output, &mesh, nthreads);
        if (rc != 0) {
            fprintf(stderr, "Error: could not write to '%s'\n", params.output);
            free(heightmap);
            trimesh_free(&mesh);
            return 1;
        }
    }

    double t3 = omp_get_wtime(); // End

    printf("perlin_time_s: %.6f\n", t1 - t0);
    printf("marching_cubes_time_s: %.6f\n", t2 - t1);
    printf("obj_export_time_s: %.6f\n", t3 - t2);
    printf("total_time_s: %.6f\n", t3 - t0);
    printf("num_triangles: %zu\n", mesh.triangle_count);
    printf("num_vertices: %zu\n", mesh.vertex_count);

    free(heightmap);
    trimesh_free(&mesh);

    return 0;
}
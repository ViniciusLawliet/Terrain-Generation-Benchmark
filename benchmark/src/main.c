#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "rapl.h"
#include "gpu_energy.h"

typedef struct {
    int run;
    int batch_id;
    int batch_size;
    double application_time_s;
    double process_wall_time_s;
    double cpu_energy_j;
    double gpu_energy_j;
    double total_energy_j;
    double edp_j_s;
    double field_time_s;
    double marching_cubes_time_s;
    double obj_export_time_s;
    long long num_triangles;
    long long num_vertices;
    int child_ok;
} RunResult;

typedef struct {
    int batch_id;
    int first_run;
    int size;
    double wall_time_s;
    double cpu_energy_j;
    double gpu_energy_j;
    double total_energy_j;
    int child_ok_all;
    int cpu_energy_valid;
    int gpu_energy_valid;
    int total_energy_valid;
} BatchResult;

static double now_seconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return NAN;
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void mean_stddev(const double *values, int n, double *mean_out, double *stddev_out) {
    int valid = 0;
    double sum = 0.0;

    for (int i = 0; i < n; ++i) {
        if (isfinite(values[i])) {
            sum += values[i];
            valid++;
        }
    }

    if (valid == 0) {
        *mean_out = NAN;
        *stddev_out = NAN;
        return;
    }

    double mean = sum / (double)valid;
    double sqsum = 0.0;

    for (int i = 0; i < n; ++i) {
        if (isfinite(values[i])) {
            double d = values[i] - mean;
            sqsum += d * d;
        }
    }

    *mean_out = mean;
    *stddev_out = valid > 1
        ? sqrt(sqsum / (double)(valid - 1))
        : 0.0;
}

static void parse_child_output(const char *buf, RunResult *r) {

    r->field_time_s = NAN;
    r->marching_cubes_time_s = NAN;
    r->obj_export_time_s = NAN;
    r->application_time_s = NAN;
    r->num_triangles = -1;
    r->num_vertices = -1;

    if (!buf)
        return;

    const char *line = buf;
    while (line && *line) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);

        char linebuf[512];
        if (len >= sizeof(linebuf))
            len = sizeof(linebuf) - 1;
        memcpy(linebuf, line, len);
        linebuf[len] = '\0';

        double d;
        long long ll;

        if (sscanf(linebuf, "field_time_s: %lf", &d) == 1) {
            r->field_time_s = d;
        } else if (sscanf(linebuf, "marching_cubes_time_s: %lf", &d) == 1) {
            r->marching_cubes_time_s = d;
        } else if (sscanf(linebuf, "obj_export_time_s: %lf", &d) == 1) {
            r->obj_export_time_s = d;
        } else if (sscanf(linebuf, "total_time_s: %lf", &d) == 1) {
            r->application_time_s = d;
        } else if (sscanf(linebuf, "num_triangles: %lld", &ll) == 1) {
            r->num_triangles = ll;
        } else if (sscanf(linebuf, "num_vertices: %lld", &ll) == 1) {
            r->num_vertices = ll;
        }

        line = nl ? nl + 1 : NULL;
    }
}

static int run_child(char **target_argv, RunResult *result) {

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        return -1;
    }

    double t0 = now_seconds();
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0)
            _exit(126);
        close(pipefd[1]);
        execv(target_argv[0], target_argv);
        _exit(127);
    }

    close(pipefd[1]);

    char *outbuf = NULL;
    size_t outcap = 0;
    size_t outlen = 0;
    char chunk[4096];
    ssize_t n;

    while ((n = read(pipefd[0], chunk, sizeof(chunk))) > 0) {
        if (outlen + (size_t)n + 1 > outcap) {
            size_t newcap = outcap ? outcap * 2 : 8192;
            while (newcap < outlen + (size_t)n + 1)
                newcap *= 2;

            char *tmp = (char *)realloc(outbuf, newcap);
            if (!tmp) {
                fprintf(stderr, "Error: insufficient memory while capturing stdout.\n");
                free(outbuf);
                close(pipefd[0]);
                waitpid(pid, NULL, 0);
                return -1;
            }

            outbuf = tmp;
            outcap = newcap;
        }

        memcpy(outbuf + outlen, chunk, (size_t)n);
        outlen += (size_t)n;
        outbuf[outlen] = '\0';
    }

    close(pipefd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        free(outbuf);
        return -1;
    }

    double t1 = now_seconds();

    memset(result, 0, sizeof(*result));
    result->process_wall_time_s = t1 - t0;
    result->child_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    result->cpu_energy_j = NAN;
    result->gpu_energy_j = NAN;
    result->total_energy_j = NAN;
    result->edp_j_s = NAN;

    if (outbuf) {
        parse_child_output(outbuf, result);
        free(outbuf);
    }

    return 0;
}

static int measure_batch(char **target_argv,
                         const RaplContext *rapl,
                         GpuEnergyContext *gpu,
                         int batch_id,
                         int first_run,
                         int batch_size,
                         RunResult *results,
                         BatchResult *batch)
{
    unsigned long long rapl_before[RAPL_MAX_DOMAINS] = {0};
    unsigned long long rapl_after[RAPL_MAX_DOMAINS] = {0};

    memset(batch, 0, sizeof(*batch));
    batch->batch_id = batch_id;
    batch->first_run = first_run;
    batch->size = batch_size;
    batch->cpu_energy_j = NAN;
    batch->gpu_energy_j = NAN;
    batch->total_energy_j = NAN;
    batch->child_ok_all = 1;

    int rapl_before_ok = (rapl && rapl->available) ? rapl_read(rapl, rapl_before) : 0;

    if (gpu)
        gpu_energy_start(gpu);

    double t0 = now_seconds();

    for (int i = 0; i < batch_size; ++i) {
        if (run_child(target_argv, &results[i]) != 0) {
            if (gpu)
                (void)gpu_energy_stop_joules(gpu);
            return -1;
        }

        results[i].run = first_run + i;
        results[i].batch_id = batch_id;
        results[i].batch_size = batch_size;

        if (!results[i].child_ok)
            batch->child_ok_all = 0;
    }

    double t1 = now_seconds();
    batch->wall_time_s = t1 - t0;

    double gpu_j = gpu ? gpu_energy_stop_joules(gpu) : NAN;
    int rapl_after_ok = (rapl && rapl->available) ? rapl_read(rapl, rapl_after) : 0;

    if (rapl && rapl->available &&
        rapl_before_ok == rapl->count &&
        rapl_after_ok == rapl->count) {
        batch->cpu_energy_j = rapl_energy_joules(
            rapl, rapl_before, rapl_after);
        batch->cpu_energy_valid = isfinite(batch->cpu_energy_j);
    }

    if (gpu) {
        batch->gpu_energy_j = gpu_j;
        batch->gpu_energy_valid = gpu_energy_measurement_valid(gpu) && isfinite(gpu_j);
    }

    if (!gpu) {
        batch->total_energy_j = batch->cpu_energy_j;
        batch->total_energy_valid = batch->cpu_energy_valid;
    } else if (batch->cpu_energy_valid && batch->gpu_energy_valid) {
        batch->total_energy_j = batch->cpu_energy_j + batch->gpu_energy_j;
        batch->total_energy_valid = 1;
    }

    for (int i = 0; i < batch_size; ++i) {
        if (batch->cpu_energy_valid)
            results[i].cpu_energy_j = batch->cpu_energy_j / (double)batch_size;
        if (batch->gpu_energy_valid)
            results[i].gpu_energy_j = batch->gpu_energy_j / (double)batch_size;
        if (batch->total_energy_valid)
            results[i].total_energy_j = batch->total_energy_j / (double)batch_size;

        if (!isfinite(results[i].application_time_s))
            results[i].application_time_s = results[i].process_wall_time_s;

        if (isfinite(results[i].total_energy_j) &&
            isfinite(results[i].application_time_s)) {
            results[i].edp_j_s = results[i].total_energy_j *
                                 results[i].application_time_s;
        }
    }

    return 0;
}

static double sum_times(const RunResult *results, int n, int *all_valid) {
    double sum = 0.0;
    int ok = 1;

    for (int i = 0; i < n; ++i) {
        if (!results[i].child_ok || !isfinite(results[i].application_time_s)) {
            ok = 0;
            continue;
        }
        sum += results[i].application_time_s;
    }

    if (all_valid)
        *all_valid = ok;
    return ok ? sum : NAN;
}

static double sum_batch_energy(const BatchResult *batches, int n, int kind, int *all_valid) {
    double sum = 0.0;
    int ok = 1;

    for (int i = 0; i < n; ++i) {
        double v = NAN;

        if (!batches[i].child_ok_all) {
            ok = 0;
            continue;
        }

        if (kind == 0) v = batches[i].cpu_energy_j;
        else if (kind == 1) v = batches[i].gpu_energy_j;
        else v = batches[i].total_energy_j;

        if (!isfinite(v)) {
            ok = 0;
            continue;
        }
        sum += v;
    }

    if (all_valid)
        *all_valid = ok;
    return ok ? sum : NAN;
}

static void derive_paths(const char *prefix, char *runs, size_t runs_sz, char *summary, size_t summary_sz) {
    snprintf(runs, runs_sz, "%s_runs.csv", prefix);
    snprintf(summary, summary_sz, "%s_summary.csv", prefix);
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s -b <binary> -n <repetitions> -o <output-prefix>\n"
        "     [-g] [--gpu-index <n>] [--warmup <n>]\n"
        "     [--energy-batch <n>] -- [binary arguments]\n\n"
        "Options:\n"
        "  -b, --bin <path>          Target executable\n"
        "  -n, --reps <n>            Number of measured executions\n"
        "  -o, --output <prefix>     Prefix for the two CSV outputs\n"
        "  -g, --gpu                 Measure GPU energy via NVML\n"
        "      --gpu-index <n>       GPU index (default: 0)\n"
        "      --warmup <n>          Unmeasured warm-up executions (default: 1)\n"
        "      --energy-batch <n>    Executions per energy measurement (default: 1)\n"
        "  -h, --help                Show this help\n\n"
        "Examples:\n"
        "  %s -b ./openmp/terrain_gen_openmp -n 10 -o ../results/openmp_4t \\\n"
        "     -- -x 1024 -y 128 -z 1024 -t 4\n\n"
        "  %s -b ./cuda/terrain_gen_cuda -n 10 -g -o ../results/cuda \\\n"
        "     -- -x 1024 -y 128 -z 1024\n",
        prog, prog, prog);
}

int main(int argc, char **argv) {

    const char *bin_path = NULL;
    const char *output_prefix = NULL;
    int reps = 0;
    int gpu_index = 0;
    int use_gpu = 0;
    int warmup = 1;
    int energy_batch = 1;

    static struct option options[] = {
        {"bin", required_argument, 0, 'b'},
        {"reps", required_argument, 0, 'n'},
        {"output", required_argument, 0, 'o'},
        {"gpu", no_argument, 0, 'g'},
        {"gpu-index", required_argument, 0, 1001},
        {"warmup", required_argument, 0, 1002},
        {"energy-batch", required_argument, 0, 1003},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "b:n:o:gh", options, NULL)) != -1) {
        switch (opt) {
        case 'b': bin_path = optarg; break;
        case 'n': reps = atoi(optarg); break;
        case 'o': output_prefix = optarg; break;
        case 'g': use_gpu = 1; break;
        case 1001: gpu_index = atoi(optarg); break;
        case 1002: warmup = atoi(optarg); break;
        case 1003: energy_batch = atoi(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 1;
        }
    }

    if (!bin_path || !output_prefix || reps <= 0 || warmup < 0 || energy_batch <= 0) {
        fprintf(stderr, "Error: invalid benchmark options.\n");
        usage(argv[0]);
        return 1;
    }

    int target_argc = argc - optind;
    char **target_argv = (char **)calloc((size_t)target_argc + 2, sizeof(*target_argv));

    if (!target_argv) {
        fprintf(stderr, "Error: insufficient memory.\n");
        return 1;
    }

    target_argv[0] = (char *)bin_path;
    for (int i = 0; i < target_argc; ++i)
        target_argv[i + 1] = argv[optind + i];
    target_argv[target_argc + 1] = NULL;

    RaplContext rapl;
    rapl_init(&rapl);

    if (!rapl.available) {
        fprintf(stderr, "Warning: RAPL unavailable; CPU package energy will be N/A.\n");
        // exit(1);
    }

    GpuEnergyContext *gpu = NULL;
    if (use_gpu) {
        gpu = gpu_energy_init(gpu_index);
        if (!gpu) {
            fprintf(stderr, "Warning: GPU energy measurement unavailable.\n");
            // exit(1);
        }
    }

    for (int i = 0; i < warmup; ++i) {
        RunResult warm;
        fprintf(stderr, "Warm-up %d/%d...\n", i + 1, warmup);
        if (run_child(target_argv, &warm) != 0) {
            fprintf(stderr, "Error during warm-up.\n");
            if (gpu) gpu_energy_shutdown(gpu);
            free(target_argv);
            return 1;
        }
    }

    RunResult *results = (RunResult *)calloc((size_t)reps, sizeof(*results));
    int batch_count = (reps + energy_batch - 1) / energy_batch;
    BatchResult *batches = (BatchResult *)calloc((size_t)batch_count, sizeof(*batches));

    if (!results || !batches) {
        fprintf(stderr, "Error: insufficient memory for benchmark results.\n");
        free(results);
        free(batches);
        if (gpu) gpu_energy_shutdown(gpu);
        free(target_argv);
        return 1;
    }

    int completed = 0;
    int completed_batches = 0;

    while (completed < reps) {
        int this_batch = energy_batch;
        if (this_batch > reps - completed)
            this_batch = reps - completed;

        fprintf(stderr, "Measurement batch %d: runs %d-%d/%d...\n",
                completed_batches + 1,
                completed + 1,
                completed + this_batch,
                reps);

        if (measure_batch(target_argv, &rapl, gpu,
                          completed_batches + 1,
                          completed + 1,
                          this_batch,
                          &results[completed],
                          &batches[completed_batches]) != 0) {
            fprintf(stderr, "Error during benchmark execution.\n");
            free(results);
            free(batches);
            if (gpu) gpu_energy_shutdown(gpu);
            free(target_argv);
            return 1;
        }

        completed += this_batch;
        completed_batches++;
    }

    int successful = 0;
    for (int i = 0; i < reps; ++i) {
        if (results[i].child_ok && isfinite(results[i].application_time_s))
            successful++;
    }

    double *times = (double *)malloc((size_t)reps * sizeof(double));
    double *cpu_e = (double *)malloc((size_t)reps * sizeof(double));
    double *gpu_e = (double *)malloc((size_t)reps * sizeof(double));
    double *total_energy_values = (double *)malloc((size_t)reps * sizeof(double));
    double *edp = (double *)malloc((size_t)reps * sizeof(double));

    if (!times || !cpu_e || !gpu_e || !total_energy_values || !edp) {
        fprintf(stderr, "Error: insufficient memory for statistics.\n");
        free(times); free(cpu_e); free(gpu_e); free(total_energy_values); free(edp);
        free(results); free(batches);
        if (gpu) gpu_energy_shutdown(gpu);
        free(target_argv);
        return 1;
    }

    for (int i = 0; i < reps; ++i) {
        times[i] = results[i].child_ok ? results[i].application_time_s : NAN;
        cpu_e[i] = results[i].cpu_energy_j;
        gpu_e[i] = results[i].gpu_energy_j;
        total_energy_values[i] = results[i].total_energy_j;
        edp[i] = results[i].edp_j_s;
    }

    double mean_time = NAN, std_time = NAN;
    mean_stddev(times, reps, &mean_time, &std_time);

    double mean_cpu_e = NAN, std_cpu_e = NAN;
    double mean_gpu_e = NAN, std_gpu_e = NAN;
    double mean_total_e = NAN, std_total_e = NAN;
    double mean_edp = NAN, std_edp = NAN;

    if (energy_batch == 1) {
        mean_stddev(cpu_e, reps, &mean_cpu_e, &std_cpu_e);
        mean_stddev(gpu_e, reps, &mean_gpu_e, &std_gpu_e);
        mean_stddev(total_energy_values, reps, &mean_total_e, &std_total_e);
        mean_stddev(edp, reps, &mean_edp, &std_edp);
    } else {
        double *bcpu = (double *)malloc((size_t)batch_count * sizeof(double));
        double *bgpu = (double *)malloc((size_t)batch_count * sizeof(double));
        double *btotal = (double *)malloc((size_t)batch_count * sizeof(double));
        double *bedp = (double *)malloc((size_t)batch_count * sizeof(double));

        if (bcpu && bgpu && btotal && bedp) {
            for (int b = 0; b < batch_count; ++b) {
                if (!batches[b].child_ok_all) {
                    bcpu[b] = bgpu[b] = btotal[b] = bedp[b] = NAN;
                    continue;
                }

                int begin = batches[b].first_run - 1;
                int size = batches[b].size;
                double batch_mean_time = 0.0;
                int ok = 1;

                for (int i = 0; i < size; ++i) {
                    if (!isfinite(results[begin + i].application_time_s)) {
                        ok = 0;
                        break;
                    }
                    batch_mean_time += results[begin + i].application_time_s;
                }

                if (!ok) {
                    bcpu[b] = bgpu[b] = btotal[b] = bedp[b] = NAN;
                    continue;
                }

                batch_mean_time /= (double)size;
                bcpu[b] = batches[b].cpu_energy_valid
                    ? batches[b].cpu_energy_j / (double)size : NAN;
                bgpu[b] = batches[b].gpu_energy_valid
                    ? batches[b].gpu_energy_j / (double)size : NAN;
                btotal[b] = batches[b].total_energy_valid
                    ? batches[b].total_energy_j / (double)size : NAN;
                bedp[b] = isfinite(btotal[b])
                    ? btotal[b] * batch_mean_time : NAN;
            }

            mean_stddev(bcpu, batch_count, &mean_cpu_e, &std_cpu_e);
            mean_stddev(bgpu, batch_count, &mean_gpu_e, &std_gpu_e);
            mean_stddev(btotal, batch_count, &mean_total_e, &std_total_e);
            mean_stddev(bedp, batch_count, &mean_edp, &std_edp);
        }

        free(bcpu); free(bgpu); free(btotal); free(bedp);
    }

    int time_all_valid = 0;
    double total_time = sum_times(results, reps, &time_all_valid);

    int cpu_all_valid = 0;
    int gpu_all_valid = 0;
    int total_e_all_valid = 0;

    double total_cpu_e = sum_batch_energy(batches, batch_count, 0, &cpu_all_valid);
    double total_gpu_e = sum_batch_energy(batches, batch_count, 1, &gpu_all_valid);
    double total_energy_sum = sum_batch_energy(batches, batch_count, 2, &total_e_all_valid);

    char runs_path[4096], summary_path[4096];
    derive_paths(output_prefix, runs_path, sizeof(runs_path), summary_path, sizeof(summary_path));

    FILE *runs = fopen(runs_path, "w");
    if (!runs) {
        fprintf(stderr, "Error: cannot write '%s': %s\n", runs_path, strerror(errno));
    } else {
        fprintf(runs,
                "run,batch_id,batch_size,application_time_s,process_wall_time_s,"
                "cpu_package_energy_j,gpu_energy_j,measured_total_energy_j,edp_j_s,"
                "energy_scope,field_time_s,marching_cubes_time_s,obj_export_time_s,"
                "num_triangles,num_vertices,child_ok\n");

        for (int i = 0; i < reps; ++i) {
            fprintf(runs,
                    "%d,%d,%d,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,\"%s\","
                    "%.9f,%.9f,%.9f,%lld,%lld,%d\n",
                    results[i].run,
                    results[i].batch_id,
                    results[i].batch_size,
                    results[i].application_time_s,
                    results[i].process_wall_time_s,
                    results[i].cpu_energy_j,
                    results[i].gpu_energy_j,
                    results[i].total_energy_j,
                    results[i].edp_j_s,
                    energy_batch == 1 ? "execution" : "batch_average_estimate",
                    results[i].field_time_s,
                    results[i].marching_cubes_time_s,
                    results[i].obj_export_time_s,
                    results[i].num_triangles,
                    results[i].num_vertices,
                    results[i].child_ok);
        }
        fclose(runs);
    }

    FILE *summary = fopen(summary_path, "w");
    if (!summary) {
        fprintf(stderr, "Error: cannot write '%s': %s\n", summary_path, strerror(errno));
    } else {
        fprintf(summary,
                "repetitions,warmup,energy_batch,successful_runs,"
                "mean_time_s,stddev_time_s,total_time_s,"
                "mean_cpu_package_energy_j,stddev_cpu_package_energy_j,total_cpu_package_energy_j,"
                "mean_gpu_energy_j,stddev_gpu_energy_j,total_gpu_energy_j,"
                "mean_measured_total_energy_j,stddev_measured_total_energy_j,total_measured_energy_j,"
                "mean_edp_j_s,stddev_edp_j_s,energy_scope,gpu_method\n");

        fprintf(summary,
                "%d,%d,%d,%d,%.9f,%.9f,%.9f,"
                "%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,"
                "%.9f,%.9f,%.9f,%.9f,%.9f,\"%s\",\"%s\"\n",
                reps,
                warmup,
                energy_batch,
                successful,
                mean_time,
                std_time,
                total_time,
                mean_cpu_e,
                std_cpu_e,
                total_cpu_e,
                mean_gpu_e,
                std_gpu_e,
                total_gpu_e,
                mean_total_e,
                std_total_e,
                total_energy_sum,
                mean_edp,
                std_edp,
                energy_batch == 1 ? "per_execution" : "per_batch_average",
                gpu ? gpu_energy_method(gpu) : "not_used");

        fclose(summary);
    }

    printf("\nBENCHMARK RESULT\n");
    printf("executions_requested: %d\n", reps);
    printf("executions_valid: %d\n", successful);
    printf("warmup: %d\n", warmup);
    printf("energy_batch: %d\n", energy_batch);

    printf("\nTIME\n");
    printf("mean_time_s: %.9f\n", mean_time);
    printf("stddev_time_s: %.9f\n", std_time);
    printf("total_time_s: %.9f\n", total_time);

    printf("\nCPU PACKAGE ENERGY (RAPL)\n");
    if (cpu_all_valid) {
        printf("mean_energy_j: %.9f\n", mean_cpu_e);
        printf("stddev_energy_j: %.9f\n", std_cpu_e);
        printf("total_energy_j: %.9f\n", total_cpu_e);
    } else {
        printf("energy: unavailable or incomplete\n");
    }

    if (use_gpu) {
        printf("\nGPU ENERGY (NVML)\n");
        printf("method: %s\n", gpu ? gpu_energy_method(gpu) : "unavailable");
        if (gpu_all_valid) {
            printf("mean_energy_j: %.9f\n", mean_gpu_e);
            printf("stddev_energy_j: %.9f\n", std_gpu_e);
            printf("total_energy_j: %.9f\n", total_gpu_e);
        } else {
            printf("energy: unavailable or incomplete\n");
        }
    }

    printf("\nMEASURED TOTAL ENERGY (CPU PACKAGE + GPU)\n");
    if (total_e_all_valid) {
        printf("mean_energy_j: %.9f\n", mean_total_e);
        printf("stddev_energy_j: %.9f\n", std_total_e);
        printf("total_energy_j: %.9f\n", total_energy_sum);
    } else {
        printf("energy: unavailable or incomplete\n");
    }

    printf("\nEDP\n");
    printf("mean_edp_j_s: %.9f\n", mean_edp);
    printf("stddev_edp_j_s: %.9f\n", std_edp);

    printf("\nFILES\n");
    printf("per_run_csv: %s\n", runs_path);
    printf("summary_csv: %s\n", summary_path);

    free(times); free(cpu_e); free(gpu_e); free(total_energy_values); free(edp);
    free(results); free(batches);
    if (gpu) gpu_energy_shutdown(gpu);
    free(target_argv);

    return 0;
}
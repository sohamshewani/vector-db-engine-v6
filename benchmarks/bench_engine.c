#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <math.h>

#define VECTOR_DIM 128
#define WARMUP_ITERATIONS 1000
#define MEASURE_ITERATIONS 100000

typedef struct {
    float data[VECTOR_DIM];
} Vector;

typedef struct {
    uint64_t *latencies;
    size_t count;
    size_t capacity;
} LatencyStats;

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int compare_uint64(const void *a, const void *b) {
    uint64_t arg1 = *(const uint64_t *)a;
    uint64_t arg2 = *(const uint64_t *)b;
    if (arg1 < arg2) return -1;
    if (arg1 > arg2) return 1;
    return 0;
}

void init_stats(LatencyStats *stats, size_t capacity) {
    stats->latencies = malloc(capacity * sizeof(uint64_t));
    stats->count = 0;
    stats->capacity = capacity;
}

void free_stats(LatencyStats *stats) {
    free(stats->latencies);
}

void record_latency(LatencyStats *stats, uint64_t lat) {
    if (stats->count < stats->capacity) {
        stats->latencies[stats->count++] = lat;
    }
}

// Simulated vector distance calculation (L2 norm core loop)
static volatile float g_sink = 0.0f;
__attribute__((noinline))
void vector_engine_search(const Vector *a, const Vector *b) {
    float sum = 0.0f;
    for (int i = 0; i < VECTOR_DIM; i++) {
        float diff = a->data[i] - b->data[i];
        sum += diff * diff;
    }
    g_sink += sqrtf(sum);
}

int main(void) {
    Vector v1, v2;
    for (int i = 0; i < VECTOR_DIM; i++) {
        v1.data[i] = (float)rand() / RAND_MAX;
        v2.data[i] = (float)rand() / RAND_MAX;
    }

    LatencyStats stats;
    init_stats(&stats, MEASURE_ITERATIONS);

    // Warmup phase
    for (int i = 0; i < WARMUP_ITERATIONS; i++) {
        vector_engine_search(&v1, &v2);
    }

    // Measurement phase
    uint64_t start_global = get_time_ns();

    for (size_t i = 0; i < MEASURE_ITERATIONS; i++) {
        uint64_t start_op = get_time_ns();
        vector_engine_search(&v1, &v2);
        uint64_t end_op = get_time_ns();
        
        record_latency(&stats, end_op - start_op);
    }

    uint64_t end_global = get_time_ns();
    uint64_t total_duration_ns = end_global - start_global;

    // Calculate metrics
    qsort(stats.latencies, stats.count, sizeof(uint64_t), compare_uint64);

    uint64_t p50 = stats.latencies[(size_t)(stats.count * 0.50)];
    uint64_t p90 = stats.latencies[(size_t)(stats.count * 0.90)];
    uint64_t p99 = stats.latencies[(size_t)(stats.count * 0.99)];
    uint64_t p999 = stats.latencies[(size_t)(stats.count * 0.999)];

    double total_seconds = (double)total_duration_ns / 1e9;
    double ops_sec = (double)MEASURE_ITERATIONS / total_seconds;

    // Output formatted tabular metrics to stdout
    printf("\n");
    printf("=================================================================\n");
    printf("                  VECTOR-DB-ENGINE MICROBENCHMARK                \n");
    printf("=================================================================\n");
    printf(" Vector Dimension : %d\n", VECTOR_DIM);
    printf(" Iterations       : %d\n", MEASURE_ITERATIONS);
    printf(" Total Time       : %.6f seconds\n", total_seconds);
    printf("-----------------------------------------------------------------\n");
    printf(" Metric           | Value\n");
    printf("------------------+----------------------------------------------\n");
    printf(" Throughput       | %10.2f ops/sec\n", ops_sec);
    printf(" Latency p50      | %10.2f ns\n", (double)p50);
    printf(" Latency p90      | %10.2f ns\n", (double)p90);
    printf(" Latency p99      | %10.2f ns\n", (double)p99);
    printf(" Latency p99.9    | %10.2f ns\n", (double)p999);
    printf("=================================================================\n");
    printf("\n");

    free_stats(&stats);
    return 0;
}
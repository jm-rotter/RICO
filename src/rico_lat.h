/*
 * rico_lat.h - latency statistics over TSC-cycle samples.
 *
 * min/max/mean are exact over every sample. The median comes from a bounded
 * sample array (every `stride`-th value) so a 100M-request run doesn't need
 * 800 MB; with 1M samples the median is accurate to well under 1%.
 */
#ifndef RICO_LAT_H
#define RICO_LAT_H

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "rico_ring.h"

#define RICO_LAT_MAX_SAMPLES (1u << 20)

struct rico_lat {
    uint64_t n, min, max, sum;
    uint64_t rejected;                      /* negative / absurd deltas, excluded */
    uint64_t *samples;
    size_t   nsamples;
    uint64_t stride;
};

static inline int rico_lat_init(struct rico_lat *l, uint64_t expected)
{
    memset(l, 0, sizeof *l);
    l->min = ~0ull;
    l->stride = expected / RICO_LAT_MAX_SAMPLES + 1;
    l->samples = malloc(RICO_LAT_MAX_SAMPLES * sizeof *l->samples);
    return l->samples ? 0 : -1;
}

static inline void rico_lat_add(struct rico_lat *l, uint64_t cycles)
{
    if (cycles >> 40) { l->rejected++; return; }    /* clock skew / hostile echo */
    if (cycles < l->min) l->min = cycles;
    if (cycles > l->max) l->max = cycles;
    l->sum += cycles;
    if (l->n++ % l->stride == 0 && l->nsamples < RICO_LAT_MAX_SAMPLES)
        l->samples[l->nsamples++] = cycles;
}

static inline int rico_u64_cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* Rough TSC frequency (Hz) from a 20 ms calibration, for cycles -> ns. */
static inline double rico_tsc_hz(void)
{
    struct timespec a, b, d = { 0, 20 * 1000 * 1000 };
    clock_gettime(CLOCK_MONOTONIC, &a);
    uint64_t t0 = rico_rdtsc();
    nanosleep(&d, NULL);
    uint64_t t1 = rico_rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &b);
    double s = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) * 1e-9;
    return (t1 - t0) / s;
}

static inline uint64_t rico_lat_pct(const struct rico_lat *l, double p)
{
    size_t i = (size_t)(p * (l->nsamples - 1) + 0.5);
    return l->samples[i];
}

/* Prints stats and frees the sample buffer. hz <= 0 omits the ns column.
 * Percentiles come from the (sorted) sample array: p99.9 needs >= ~10k samples
 * to mean anything. */
static inline void rico_lat_report(const char *name, struct rico_lat *l, double hz)
{
    if (!l->n) { printf("  %s: no samples\n", name); goto out; }
    qsort(l->samples, l->nsamples, sizeof *l->samples, rico_u64_cmp);
    double avg = (double)l->sum / l->n;
    uint64_t p50 = rico_lat_pct(l, 0.50), p90 = rico_lat_pct(l, 0.90),
             p99 = rico_lat_pct(l, 0.99), p999 = rico_lat_pct(l, 0.999);
    printf("  %s (n=%llu, cycles%s)\n", name, (unsigned long long)l->n,
           l->rejected ? ", some rejected" : "");
    printf("    min %llu | p50 %llu | p90 %llu | p99 %llu | p99.9 %llu | max %llu | avg %.1f\n",
           (unsigned long long)l->min, (unsigned long long)p50, (unsigned long long)p90,
           (unsigned long long)p99, (unsigned long long)p999,
           (unsigned long long)l->max, avg);
    if (hz > 0)
        printf("    p50 %.0f ns | p99 %.0f ns | avg %.0f ns  (TSC ~%.2f GHz)\n",
               p50 / hz * 1e9, p99 / hz * 1e9, avg / hz * 1e9, hz / 1e9);
    if (l->rejected) printf("    %llu samples rejected (skew / hostile echo)\n",
                            (unsigned long long)l->rejected);
out:
    free(l->samples);
    l->samples = NULL;
}

#endif

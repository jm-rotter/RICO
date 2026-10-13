/*
 * rico_pin.h - pin the calling thread (or single-threaded process) to one CPU
 * to remove scheduler-migration jitter from measurements.
 *
 * Pick two *different physical cores* for guest and host (not SMT siblings of
 * each other, unless that's what you want to measure); `lscpu -e` shows the
 * core/CPU map. Combine with `isolcpus=`/`nohz_full=` for the quietest runs.
 */
#ifndef RICO_PIN_H
#define RICO_PIN_H

#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>

/* cpu < 0: no-op. Returns 0 on success, an errno value on failure. */
static inline int rico_pin_self(int cpu)
{
    if (cpu < 0) return 0;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof set, &set);
    if (rc) fprintf(stderr, "rico: cannot pin to cpu %d (errno %d)\n", cpu, rc);
    return rc;
}

#endif

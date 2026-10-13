/*
 * rico_test.c - bidirectional (SQ + CQ) correctness/load test.
 *
 * Host side = the real rico_host worker thread on a mock block device.
 * Guest side = rico_guest_run(), either as a thread (works under TSan) or as a
 * forked child that maps the backing file independently (--procs), the closest
 * user-space analogue of a separate VM.
 * Pass criteria: every request completed in order, every READ returned the
 * bytes the guest's model expects, zero host-side workload mismatches.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "rico_guest.h"
#include "rico_host.h"
#include "rico_mockdev.h"

static struct rico_mockdev md;

struct garg { struct rico_shm *s; struct rico_guest_cfg cfg; struct rico_guest_result r; };

static void *guest_thread(void *p)
{
    struct garg *g = p;
    rico_guest_run(g->s, &g->cfg, &g->r);
    return NULL;
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    struct rico_guest_cfg cfg = { .n = 20u * 1000 * 1000, .depth = 0, .warmup = (uint64_t)-1,
                                  .cq_leg = 1, .cpu = -1 };
    int procs = 0, host_cpu = -1, busy = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--procs")) procs = 1;
        else if (!strcmp(argv[i], "--pingpong")) cfg.depth = 1;
        else if (!strcmp(argv[i], "--depth") && i + 1 < argc) cfg.depth = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--guest-cpu") && i + 1 < argc) cfg.cpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--host-cpu") && i + 1 < argc) host_cpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) cfg.warmup = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--busy")) busy = 1;
        else if (argv[i][0] == '-') { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
        else cfg.n = strtoull(argv[i], NULL, 0);
    }
    if (cfg.depth == 1) busy = 1;           /* ping-pong measures latency: never let the host sleep */
    if (cfg.warmup == (uint64_t)-1) cfg.warmup = cfg.n / 10 > 10000 ? 10000 : cfg.n / 10;
    uint64_t n = cfg.n;

    char path[64];
    snprintf(path, sizeof path, "/dev/shm/rico_test.%d", getpid());
    struct rico_host h;
    if (rico_host_create(path) || rico_host_open(&h, path, rico_mockdev_handle, &md, host_cpu)) {
        perror("host setup"); return 2;
    }

    double t0 = now();
    h.busy_poll = busy;
    rico_host_start(&h);

    struct rico_guest_result r = {0};
    if (procs) {
        pid_t pid = fork();
        if (pid == 0) {                     /* guest: own mapping of the file */
            int fd = open(path, O_RDWR);
            struct rico_shm *s = mmap(NULL, RICO_SHM_SIZE, PROT_READ | PROT_WRITE,
                                      MAP_SHARED, fd, 0);
            struct rico_guest_result gr;
            uint64_t e = rico_guest_run(s, &cfg, &gr);
            rico_guest_report(&cfg, &gr, rico_tsc_hz());
            fflush(stdout);
            _exit(e ? 1 : 0);
        }
        int st; waitpid(pid, &st, 0);
        r.completed = n; r.errors = (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : 1;
    } else {
        struct garg g = { h.shm, cfg, {0} };
        pthread_t t;
        pthread_create(&t, NULL, guest_thread, &g);
        pthread_join(t, NULL);
        r = g.r;
        rico_guest_report(&cfg, &g.r, rico_tsc_hz());
    }
    double dt = now() - t0;

    rico_host_stop(&h);
    rico_host_close(&h);
    unlink(path);

    int ok = r.completed == n && r.errors == 0 && h.completed == n &&
             h.consumed == n && md.errors == 0 && h.bad_requests == 0;
    printf("mode=%s requests=%llu host: consumed=%llu completed=%llu errors=%llu | guest errors=%llu\n",
           procs ? "procs" : "threads", (unsigned long long)n,
           (unsigned long long)h.consumed, (unsigned long long)h.completed,
           (unsigned long long)md.errors, (unsigned long long)r.errors);
    printf("%.2f s total -> %s\n", dt, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

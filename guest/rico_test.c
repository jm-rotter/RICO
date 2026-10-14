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
#ifdef RICO_HAVE_IOURING
#include "rico_iouring.h"
#endif

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
    int procs = 0, host_cpu = -1, busy = 0, use_uring = 0, sqpoll = 0, no_fixed = 0;
    unsigned qd = 0;
    const char *backing = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--procs")) procs = 1;
        else if (!strcmp(argv[i], "--pingpong")) cfg.depth = 1;
        else if (!strcmp(argv[i], "--depth") && i + 1 < argc) cfg.depth = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--guest-cpu") && i + 1 < argc) cfg.cpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--host-cpu") && i + 1 < argc) host_cpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) cfg.warmup = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--busy")) busy = 1;
        else if (!strcmp(argv[i], "--iouring")) use_uring = 1;
        else if (!strcmp(argv[i], "--backing") && i + 1 < argc) { backing = argv[++i]; use_uring = 1; }
        else if (!strcmp(argv[i], "--qd") && i + 1 < argc) qd = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sqpoll")) { sqpoll = 1; use_uring = 1; }
        else if (!strcmp(argv[i], "--no-fixed")) no_fixed = 1;
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

#ifdef RICO_HAVE_IOURING
    struct rico_iouring *io = NULL;
    char img[64];
    if (use_uring) {
        if (!backing) {         /* tmpfs by default: the workload is 1/8 FLUSH, which would be
                                 * an fsync storm on a real disk */
            snprintf(img, sizeof img, "/dev/shm/rico_test.%d.img", getpid());
            backing = img;
        }
        struct rico_iouring_cfg icfg = { .path = backing, .min_size = RICO_DEV_BYTES, .qd = qd,
                                         .sqpoll = sqpoll, .sqpoll_cpu = -1, .no_fixed = no_fixed };
        int err;
        io = rico_iouring_create(&icfg, &err);
        if (!io) { fprintf(stderr, "io_uring backend: %s\n", strerror(-err)); return 2; }
        rico_host_set_backend(&h, &rico_iouring_ops, io);
        printf("backend: io_uring on %s (%s)\n", backing, rico_iouring_describe(io));
    }
#else
    if (use_uring) { fprintf(stderr, "built without liburing\n"); return 2; }
#endif

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

#ifdef RICO_HAVE_IOURING
    if (io) {
        /* Independent end-to-end check of what actually reached the device: replay
         * every write in submission order and compare with the file's final bytes
         * (valid because overlapping requests are executed in submission order). */
        const struct rico_iouring_stats *st = rico_iouring_get_stats(io);
        printf("iouring: submitted=%llu kicks=%llu (%.1f sqes/kick) conflict_stalls=%llu slot_stalls=%llu "
               "short=%llu io_errors=%llu bad_cqes=%llu max_inflight=%llu\n",
               (unsigned long long)st->submitted, (unsigned long long)st->kicks,
               st->kicks ? (double)st->sqes / st->kicks : 0.0,
               (unsigned long long)st->conflict_stalls, (unsigned long long)st->slot_stalls,
               (unsigned long long)st->short_ios, (unsigned long long)st->io_errors,
               (unsigned long long)st->bad_cqes, (unsigned long long)st->max_inflight);
        int clean = !st->io_errors && !st->bad_cqes && !st->submit_errors && !st->range_rejects;
        rico_iouring_destroy(io);
        uint8_t *model = calloc(1, RICO_DEV_BYTES), *file = malloc(RICO_DEV_BYTES);
        int fd = open(backing, O_RDONLY);
        int match = 0;
        if (model && file && fd >= 0) {
            struct rico_req q;
            for (uint64_t id = 0; id < n; id++) {
                rico_make_req(id, &q);
                if (q.op == RICO_OP_WRITE) memcpy(&model[q.offset], q.payload, q.len);
            }
            match = pread(fd, file, RICO_DEV_BYTES, 0) == (ssize_t)RICO_DEV_BYTES &&
                    !memcmp(model, file, RICO_DEV_BYTES);
        }
        printf("backing file vs replayed model: %s\n", match ? "identical" : "MISMATCH");
        if (fd >= 0) close(fd);
        if (backing == img) unlink(img);
        free(model); free(file);
        ok = ok && clean && match;
    }
#endif
    printf("mode=%s%s requests=%llu host: consumed=%llu completed=%llu errors=%llu | guest errors=%llu\n",
           procs ? "procs" : "threads", use_uring ? "+iouring" : "", (unsigned long long)n,
           (unsigned long long)h.consumed, (unsigned long long)h.completed,
           (unsigned long long)md.errors, (unsigned long long)r.errors);
    printf("%.2f s total -> %s\n", dt, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

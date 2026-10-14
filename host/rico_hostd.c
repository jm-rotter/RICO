/*
 * rico_hostd.c - standalone host daemon: creates the shared file, runs the
 * worker thread, serves requests and posts completions.
 *
 *   rico_hostd <shm-path> [expected_requests] [options]
 *     --cpu N        pin the worker thread to CPU N
 *     --busy         never sleep when idle (use for latency measurements)
 *     --backing PATH serve I/O from this file/block device through io_uring
 *                    (default: in-memory mock device, synchronous)
 *       --qd N         max requests in flight (power of two, default 256)
 *       --sqpoll       kernel SQ-poll thread (one more busy core, no submit syscalls)
 *       --sqpoll-cpu N pin that thread
 *       --no-fixed     don't register the file/buffers with io_uring
 *
 * Start this BEFORE QEMU (QEMU's memory-backend-file needs the file to exist).
 * With expected_requests it exits once that many have been consumed;
 * otherwise it runs until SIGINT.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rico_host.h"
#include "rico_mockdev.h"
#ifdef RICO_HAVE_IOURING
#include "rico_iouring.h"
#endif

static volatile sig_atomic_t quit;
static void on_sig(int s) { (void)s; quit = 1; }

static struct rico_mockdev md;              /* host-private, not shared */

int main(int argc, char **argv)
{
    const char *path = NULL;
    uint64_t want = 0;
    int cpu = -1, busy = 0, npos = 0;
    const char *backing = NULL;
    unsigned qd = 0;
    int sqpoll = 0, sqpoll_cpu = -1, no_fixed = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cpu") && i + 1 < argc) cpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--busy")) busy = 1;
        else if (!strcmp(argv[i], "--backing") && i + 1 < argc) backing = argv[++i];
        else if (!strcmp(argv[i], "--qd") && i + 1 < argc) qd = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sqpoll")) sqpoll = 1;
        else if (!strcmp(argv[i], "--sqpoll-cpu") && i + 1 < argc) sqpoll_cpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-fixed")) no_fixed = 1;
        else if (npos++ == 0) path = argv[i];
        else want = strtoull(argv[i], NULL, 0);
    }
    if (!path) { fprintf(stderr, "usage: %s <shm-path> [n] [--cpu N] [--busy] [--backing PATH [--qd N] [--sqpoll] [--sqpoll-cpu N] [--no-fixed]]\n", argv[0]); return 2; }

    if (rico_host_create(path)) { perror("create"); return 1; }
    struct rico_host h;
    if (rico_host_open(&h, path, rico_mockdev_handle, &md, cpu)) { perror("open"); return 1; }
    h.busy_poll = busy;
#ifdef RICO_HAVE_IOURING
    struct rico_iouring *io = NULL;
    if (backing) {
        struct rico_iouring_cfg cfg = { .path = backing, .min_size = RICO_DEV_BYTES, .qd = qd,
                                        .sqpoll = sqpoll, .sqpoll_cpu = sqpoll_cpu,
                                        .no_fixed = no_fixed };
        int err;
        io = rico_iouring_create(&cfg, &err);
        if (!io) { fprintf(stderr, "rico_hostd: io_uring backend on %s: %s\n", backing, strerror(-err)); return 1; }
        rico_host_set_backend(&h, &rico_iouring_ops, io);
        fprintf(stderr, "rico_hostd: io_uring backend: %s (%s)\n", backing, rico_iouring_describe(io));
    }
#else
    (void)qd; (void)sqpoll; (void)sqpoll_cpu; (void)no_fixed;
    if (backing) { fprintf(stderr, "rico_hostd: built without liburing (--backing unavailable)\n"); return 2; }
#endif
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    if (rico_host_start(&h)) { perror("start"); return 1; }
    fprintf(stderr, "rico_hostd: ready on %s (SQ %u, CQ %u slots)\n",
            path, RICO_RING_SLOTS, RICO_CQ_SLOTS);

    /* completed (not consumed) is the right exit condition: the last
     * completion must be posted before we tear the worker down. */
    while (!quit && !h.ring_corrupt) {
        if (want && __atomic_load_n(&h.completed, __ATOMIC_RELAXED) >= want) break;
        usleep(10000);
    }
    rico_host_stop(&h);
    printf("consumed=%llu completed=%llu bad=%llu errors=%llu%s\n",
           (unsigned long long)h.consumed, (unsigned long long)h.completed,
           (unsigned long long)h.bad_requests, (unsigned long long)md.errors,
           h.ring_corrupt ? " RING_CORRUPT" : "");
#ifdef RICO_HAVE_IOURING
    int io_ok = 1;
    if (io) {
        const struct rico_iouring_stats *st = rico_iouring_get_stats(io);
        printf("iouring: submitted=%llu completed=%llu kicks=%llu sqes=%llu (%.1f/kick) conflict_stalls=%llu "
               "slot_stalls=%llu short=%llu retries=%llu range_rejects=%llu io_errors=%llu bad_cqes=%llu "
               "submit_errors=%llu max_inflight=%llu\n",
               (unsigned long long)st->submitted, (unsigned long long)st->completed,
               (unsigned long long)st->kicks, (unsigned long long)st->sqes,
               st->kicks ? (double)st->sqes / st->kicks : 0.0,
               (unsigned long long)st->conflict_stalls, (unsigned long long)st->slot_stalls,
               (unsigned long long)st->short_ios, (unsigned long long)st->retries,
               (unsigned long long)st->range_rejects, (unsigned long long)st->io_errors,
               (unsigned long long)st->bad_cqes, (unsigned long long)st->submit_errors,
               (unsigned long long)st->max_inflight);
        io_ok = !st->io_errors && !st->bad_cqes && !st->submit_errors && !st->range_rejects;
        rico_iouring_destroy(io);           /* cancels + drains before freeing buffers */
    }
#else
    int io_ok = 1;
#endif
    int ok = io_ok && !h.ring_corrupt && !h.bad_requests && !md.errors &&
             (!want || h.completed == want);
    rico_host_close(&h);
    return ok ? 0 : 1;
}

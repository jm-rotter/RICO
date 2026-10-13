/*
 * rico_hostd.c - standalone host daemon: creates the shared file, runs the
 * worker thread, serves requests from a mock block device and posts
 * completions.
 *
 *   rico_hostd <shm-path> [expected_requests] [--cpu N] [--busy]
 *     --cpu N  pin the worker thread to CPU N
 *     --busy   never sleep when idle (use for latency measurements)
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

static volatile sig_atomic_t quit;
static void on_sig(int s) { (void)s; quit = 1; }

static struct rico_mockdev md;              /* host-private, not shared */

int main(int argc, char **argv)
{
    const char *path = NULL;
    uint64_t want = 0;
    int cpu = -1, busy = 0, npos = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cpu") && i + 1 < argc) cpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--busy")) busy = 1;
        else if (npos++ == 0) path = argv[i];
        else want = strtoull(argv[i], NULL, 0);
    }
    if (!path) { fprintf(stderr, "usage: %s <shm-path> [n] [--cpu N] [--busy]\n", argv[0]); return 2; }

    if (rico_host_create(path)) { perror("create"); return 1; }
    struct rico_host h;
    if (rico_host_open(&h, path, rico_mockdev_handle, &md, cpu)) { perror("open"); return 1; }
    h.busy_poll = busy;
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
    int ok = !h.ring_corrupt && !h.bad_requests && !md.errors &&
             (!want || h.completed == want);
    rico_host_close(&h);
    return ok ? 0 : 1;
}

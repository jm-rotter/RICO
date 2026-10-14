/*
 * rico_host.h - host-side RICO worker: attaches to the shared region and drains
 * the ring on a dedicated POSIX thread.
 *
 * Embedding in QEMU: nothing here touches QEMU internals. Call
 * rico_host_open() from the device's realize(), rico_host_start() to launch the
 * worker (or run rico_host_run() from a thread created with
 * qemu_thread_create()), and rico_host_stop() from unrealize(). The worker never
 * takes the BQL and never calls into QEMU, so the main loop is not blocked.
 * Completions are pushed to the CQ by the worker itself (polled by the guest); no
 * interrupt is raised.
 */
#ifndef RICO_HOST_H
#define RICO_HOST_H

#include <pthread.h>
#include "rico_backend.h"
#include "rico_shm.h"

/*
 * Called on the worker thread for every valid request. req is a private copy.
 * cpl arrives zeroed with cpl->id already set; the handler fills status and
 * (for READ) len/data. The worker then pushes it on the CQ. Zeroing first
 * means no stale host memory can leak to the guest through unused bytes.
 */
/* Synchronous convenience backend: handler fills cpl immediately. */
typedef void (*rico_handler_fn)(const struct rico_req *req,
                                struct rico_cpl *cpl, void *opaque);

struct rico_host {
    struct rico_shm *shm;
    size_t           map_len;
    int              fd;
    rico_handler_fn  handler;               /* used by the built-in sync backend */
    void            *opaque;
    const struct rico_backend_ops *ops;     /* active backend (default: sync handler) */
    void            *be;
    pthread_t        thread;
    int              started;
    _Atomic int      stop;
    int              cpu;                   /* -1 = don't pin */
    int              busy_poll;             /* set before start: never sleep when idle.
                                             * Needed for latency runs (a sleeping
                                             * worker adds ~50us wakeups); burns a core. */
    /* worker-only counters, valid after rico_host_stop() */
    uint64_t         consumed, completed, bad_requests;
    int              ring_corrupt;
};

/* Replace the default synchronous backend (call after open, before start). */
void rico_host_set_backend(struct rico_host *h, const struct rico_backend_ops *ops, void *be);

/* Create + size + initialise the backing file (run BEFORE launching QEMU). */
int rico_host_create(const char *path);
/* Attach to an existing, initialised region. 0 on success, -1 on error. */
int rico_host_open(struct rico_host *h, const char *path,
                   rico_handler_fn fn, void *opaque, int cpu);
int rico_host_start(struct rico_host *h);
void rico_host_stop(struct rico_host *h);   /* joins worker */
void rico_host_close(struct rico_host *h);
void *rico_host_run(void *h);               /* the polling loop itself */

#endif

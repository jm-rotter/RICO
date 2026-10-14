/*
 * rico_backend.h - interface between the RICO host worker and a storage backend.
 *
 * The worker owns both rings; a backend only ever sees copied, op/len-validated
 * requests and hands back completions. All callbacks run on the worker thread
 * (no locking needed inside a backend).
 *
 * Ordering contract exposed to the guest: requests whose byte ranges overlap
 * (at least one a write) execute in submission order; everything else may
 * complete in any order, so completions can arrive out of order. The guest
 * matches completions by `id`, never by position.
 *
 * Request lifecycle through submit():
 *   RICO_BE_DONE     cpl is filled in; the worker queues it for the CQ now.
 *   RICO_BE_PENDING  accepted; it will come back later through reap().
 *   RICO_BE_BUSY     cannot accept right now (no slot, range conflict, ...).
 *                    Nothing was consumed; the worker retries the SAME request
 *                    after it has kicked and reaped.
 */
#ifndef RICO_BACKEND_H
#define RICO_BACKEND_H

#include "rico_shm.h"

enum { RICO_BE_DONE = 0, RICO_BE_PENDING = 1, RICO_BE_BUSY = 2 };

struct rico_backend_ops {
    /* cpl arrives zeroed with id and tsc_submit already set. Required. */
    int      (*submit)(void *be, const struct rico_req *req, struct rico_cpl *cpl);
    /* Hand everything accepted so far to the kernel/device (batching). Optional. */
    void     (*kick)(void *be);
    /* Non-blocking: move up to max finished requests into out[]; returns count.
     * out[i] must carry the id/tsc_submit given at submit(). Optional (sync backends). */
    unsigned (*reap)(void *be, struct rico_cpl *out, unsigned max);
    /* Requests accepted (PENDING) but not yet returned by reap(). Optional. */
    unsigned (*inflight)(void *be);
    /* Block for at most timeout_us waiting for a completion. Optional. */
    void     (*wait)(void *be, unsigned timeout_us);
};

#endif

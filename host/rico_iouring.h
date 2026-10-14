/*
 * rico_iouring.h - io_uring storage backend for the RICO host worker.
 *
 * Maps RICO requests onto a real file or block device:
 *   RICO_OP_READ  -> IORING_OP_READ(_FIXED)   into a per-request host buffer
 *   RICO_OP_WRITE -> IORING_OP_WRITE(_FIXED)  from a per-request host buffer
 *   RICO_OP_FLUSH -> IORING_OP_FSYNC, issued only once every earlier request
 *                    has completed (a host-side barrier)
 * Completions are reaped from the kernel CQ and returned to the worker, which
 * posts them on the RICO CQ.
 *
 * Semantics the guest can rely on (see rico_backend.h): overlapping requests
 * (at least one write) run in submission order; others may complete out of
 * order. A WRITE completion means "accepted by the page cache", not durable;
 * FLUSH is the durability point.
 *
 * Threading: create/destroy from any one thread while the worker is stopped;
 * everything else runs on the worker thread only.
 */
#ifndef RICO_IOURING_H
#define RICO_IOURING_H

#include <stdint.h>
#include "rico_backend.h"

struct rico_iouring_cfg {
    const char *path;           /* file or block device, opened O_RDWR */
    uint64_t    min_size;       /* regular files are created if missing and grown to at
                                 * at least this size (0 = open as-is, never create) */
    unsigned    qd;             /* max requests in flight: power of two, 1..1024; 0 = 256 */
    int         sqpoll;         /* kernel SQ-poll thread: no io_uring_enter() to submit,
                                 * costs one extra busy core */
    int         sqpoll_cpu;     /* pin that thread (-1 = any) */
    unsigned    sqpoll_idle_ms; /* SQ thread sleeps after this long idle (0 = 2) */
    int         no_fixed;       /* skip registered file/buffers (benchmarking) */
};

struct rico_iouring_stats {
    uint64_t submitted;         /* requests accepted */
    uint64_t completed;         /* requests returned to the worker */
    uint64_t kicks, sqes;       /* io_uring_submit() calls / SQEs they carried => batching */
    uint64_t conflict_stalls;   /* requests held back: range conflict, or FLUSH waiting for a drain */
    uint64_t slot_stalls;       /* requests held back: all qd slots in use */
    uint64_t short_ios, retries;/* partial transfers continued / EAGAIN-EINTR retried */
    uint64_t range_rejects;     /* offset/len outside the device -> RICO_ST_INVAL */
    uint64_t io_errors;         /* completed with RICO_ST_IOERR */
    uint64_t bad_cqes;          /* CQE whose tag matched no in-flight slot (should be 0) */
    uint64_t submit_errors;     /* io_uring_submit() failures other than EINTR (should be 0) */
    uint64_t max_inflight;
};

struct rico_iouring;

/* Returns NULL and sets *err (negative errno) on failure. */
struct rico_iouring *rico_iouring_create(const struct rico_iouring_cfg *cfg, int *err);
/* Cancels and waits for in-flight I/O before freeing anything the kernel may touch. */
void rico_iouring_destroy(struct rico_iouring *b);

uint64_t rico_iouring_size(const struct rico_iouring *b);
const struct rico_iouring_stats *rico_iouring_get_stats(const struct rico_iouring *b);
const char *rico_iouring_describe(const struct rico_iouring *b);   /* "qd=256 fixed-files fixed-bufs ..." */

extern const struct rico_backend_ops rico_iouring_ops;

#endif

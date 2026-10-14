/*
 * rico_iouring.c - io_uring backend for the RICO host worker. See rico_iouring.h
 * for the contract and the design notes below for the mechanics.
 *
 * Design notes
 * ------------
 * Slots.  Every in-flight request owns one slot: its metadata (RICO id, TSC
 *   stamp, offset, len, progress) and a RICO_MAX_PAYLOAD-byte I/O buffer. The
 *   kernel reads/writes that buffer, so it must outlive the request; the popped
 *   rico_req (a stack copy) does not. `qd` slots bound the in-flight set, and
 *   with an SQ of `qd` entries and a CQ of 2*qd the kernel CQ cannot overflow.
 *
 * user_data.  sqe->user_data = (rico_id << 16) | slot_index. The slot index is
 *   what we actually dereference; the id half is a cross-check. Guest-supplied
 *   ids are untrusted (a hostile guest can repeat them), so the id is never used
 *   as a lookup key. A CQE whose slot is not in flight or whose id disagrees is
 *   counted in bad_cqes and ignored, never acted on.
 *
 * Ordering.  Overlapping ranges (at least one write) are serialised: submit()
 *   answers BUSY while an in-flight request conflicts, and the worker retries the
 *   same request after reaping. The scan walks only the active list, so it costs
 *   O(in-flight) -- one entry in ping-pong. FLUSH is a barrier: it is held BUSY
 *   until nothing is in flight, then submitted as a plain fsync. (IOSQE_IO_DRAIN
 *   would do the same inside the kernel, but measured ~20x slower on the test
 *   workload (1.7 vs 0.08 Mreq/s) because drained SQEs get deferred and
 *   re-issued asynchronously.)
 *
 * Batching.  submit() only *prepares* an SQE. The worker calls kick() once per
 *   ingest batch (<= RICO_HOST_BATCH), so one io_uring_enter() carries many
 *   requests; with SQPOLL even that disappears.
 *
 * Reaping.  reap() drains the CQ without blocking, finishes requests (partial
 *   transfers and EAGAIN/EINTR are continued in place), and returns RICO
 *   completions. COOP_TASKRUN avoids interrupting the worker with task_work, so
 *   we must run it ourselves when the kernel flags it (IORING_SQ_TASKRUN).
 *
 * Teardown.  The kernel may still be writing into slot buffers, so destroy()
 *   cancels and waits for in-flight I/O first. If that times out it leaks the
 *   buffers rather than risk a use-after-free.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "rico_iouring.h"

#define SLOT_BITS     16
#define ID_MASK       ((1ull << (64 - SLOT_BITS)) - 1)
#define TAG(id, slot) ((((uint64_t)(id) & ID_MASK) << SLOT_BITS) | (slot))
#define TAG_CANCEL    (~0ull)           /* slot field 0xffff: never a real slot (qd <= 1024) */
#define MAX_QD        1024u
#define MAX_RETRIES   4u
#define DRAIN_TIMEOUT_MS 5000

enum { S_FREE = 0, S_INFLIGHT = 1 };

struct slot {
    uint64_t id, tsc_submit, offset;
    uint32_t len, done;             /* done: bytes already transferred */
    uint16_t active_pos;
    uint8_t  op, state, retries;
};

struct rico_iouring {
    struct io_uring ring;
    int      ring_ok;
    int      fd;
    int      fixed_file, fixed_bufs, sqpoll;
    uint64_t dev_size;
    unsigned nslots;
    struct slot *slots;
    uint8_t     *bufs;              /* nslots * RICO_MAX_PAYLOAD, kernel-visible */
    uint16_t    *free_stack;  unsigned nfree;
    uint16_t    *active;      unsigned nactive;
    unsigned     unsubmitted;       /* SQEs prepared but not yet io_uring_submit()ed */
    uint64_t     stall_id; int stalling;    /* de-dup of stall statistics */
    char desc[128];
    struct rico_iouring_stats st;
};

static inline uint8_t *slot_buf(struct rico_iouring *b, unsigned idx)
{
    return b->bufs + (size_t)idx * RICO_MAX_PAYLOAD;
}

/* ---- submission ------------------------------------------------------------ */

static void do_kick(struct rico_iouring *b)
{
    if (!b->unsubmitted) return;
    int n;
    do n = io_uring_submit(&b->ring); while (n == -EINTR);
    if (n > 0) {
        b->st.kicks++;
        b->st.sqes += (unsigned)n;
        b->unsubmitted -= (unsigned)n < b->unsubmitted ? (unsigned)n : b->unsubmitted;
    } else if (n < 0 && n != -EBUSY && n != -EAGAIN) {
        /* -EBUSY/-EAGAIN: kernel CQ backlog, clears once we reap. Anything else
         * is a real fault; count it so it can't hide. */
        b->st.submit_errors++;
    }
}

/* An SQE, flushing the SQ once if it is full. NULL only if still full. */
static struct io_uring_sqe *get_sqe(struct rico_iouring *b)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&b->ring);
    if (!sqe) {
        do_kick(b);
        sqe = io_uring_get_sqe(&b->ring);
    }
    return sqe;
}

/* Prepare (or re-prepare, for the unfinished tail) the read/write of a slot. */
static int prep_io(struct rico_iouring *b, unsigned idx, const struct slot *s)
{
    struct io_uring_sqe *sqe = get_sqe(b);
    if (!sqe) return -EBUSY;
    uint8_t *buf = slot_buf(b, idx) + s->done;
    unsigned n = s->len - s->done;
    uint64_t off = s->offset + s->done;
    int fd = b->fixed_file ? 0 : b->fd;     /* index into the registered file table */

    if (s->op == RICO_OP_READ) {
        if (b->fixed_bufs) io_uring_prep_read_fixed(sqe, fd, buf, n, off, (int)idx);
        else               io_uring_prep_read(sqe, fd, buf, n, off);
    } else {
        if (b->fixed_bufs) io_uring_prep_write_fixed(sqe, fd, buf, n, off, (int)idx);
        else               io_uring_prep_write(sqe, fd, buf, n, off);
    }
    if (b->fixed_file) io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
    io_uring_sqe_set_data64(sqe, TAG(s->id, idx));
    b->unsubmitted++;
    return 0;
}

static int prep_flush(struct rico_iouring *b, unsigned idx, const struct slot *s)
{
    struct io_uring_sqe *sqe = get_sqe(b);
    if (!sqe) return -EBUSY;
    io_uring_prep_fsync(sqe, b->fixed_file ? 0 : b->fd, 0);
    if (b->fixed_file) io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
    io_uring_sqe_set_data64(sqe, TAG(s->id, idx));
    b->unsubmitted++;
    return 0;
}

/* Does q overlap an in-flight request with at least one writer? */
static int conflicts(const struct rico_iouring *b, const struct rico_req *q)
{
    for (unsigned i = 0; i < b->nactive; i++) {
        const struct slot *a = &b->slots[b->active[i]];
        if (a->op == RICO_OP_FLUSH) continue;
        if (q->op == RICO_OP_READ && a->op == RICO_OP_READ) continue;
        if (q->offset < a->offset + a->len && a->offset < q->offset + q->len)
            return 1;
    }
    return 0;
}

static int be_submit(void *be, const struct rico_req *q, struct rico_cpl *cpl)
{
    struct rico_iouring *b = be;

    if (q->op != RICO_OP_FLUSH) {
        /* offset/len come from the guest: check without overflowing. */
        if (q->len > RICO_MAX_PAYLOAD || q->len > b->dev_size ||
            q->offset > b->dev_size - q->len) {
            b->st.range_rejects++;
            cpl->status = RICO_ST_INVAL;
            return RICO_BE_DONE;
        }
        if (q->len == 0) { cpl->status = RICO_ST_OK; return RICO_BE_DONE; }
    }
    /* Stall counters count requests, not retries: a stalled request is retried
     * every worker iteration and must only be counted the first time. */
    int stalled = 0;
    if (!b->nfree) stalled = 1, b->st.slot_stalls += b->stall_id != q->id || !b->stalling;
    else if (q->op == RICO_OP_FLUSH ? b->nactive != 0 : conflicts(b, q))
        stalled = 1, b->st.conflict_stalls += b->stall_id != q->id || !b->stalling;
    if (stalled) { b->stall_id = q->id; b->stalling = 1; return RICO_BE_BUSY; }
    b->stalling = 0;

    unsigned idx = b->free_stack[b->nfree - 1];
    struct slot *s = &b->slots[idx];
    s->id = q->id; s->tsc_submit = q->tsc_submit;
    s->offset = q->offset; s->len = q->len; s->op = (uint8_t)q->op;
    s->done = 0; s->retries = 0;
    if (q->op == RICO_OP_WRITE) memcpy(slot_buf(b, idx), q->payload, q->len);

    int rc = q->op == RICO_OP_FLUSH ? prep_flush(b, idx, s) : prep_io(b, idx, s);
    if (rc) return RICO_BE_BUSY;            /* no SQE even after a flush: retry later */

    b->nfree--;
    s->state = S_INFLIGHT;
    s->active_pos = (uint16_t)b->nactive;
    b->active[b->nactive++] = (uint16_t)idx;
    b->st.submitted++;
    if (b->nactive > b->st.max_inflight) b->st.max_inflight = b->nactive;
    return RICO_BE_PENDING;
}

static void be_kick(void *be) { do_kick(be); }

/* ---- completion ------------------------------------------------------------ */

/* Returns 1 and fills *out when the request is finished, 0 if it was continued. */
static int finish(struct rico_iouring *b, unsigned idx, struct slot *s, int res,
                  struct rico_cpl *out)
{
    uint32_t status = RICO_ST_OK;

    if (s->op == RICO_OP_FLUSH) {
        if (res < 0) status = RICO_ST_IOERR;
    } else if (res < 0) {
        if ((res == -EAGAIN || res == -EINTR) && s->retries < MAX_RETRIES) {
            s->retries++; b->st.retries++;
            if (prep_io(b, idx, s) == 0) return 0;
        }
        status = RICO_ST_IOERR;
    } else if (res == 0) {
        if (s->op == RICO_OP_READ) {            /* past EOF: a hole reads as zeros */
            memset(slot_buf(b, idx) + s->done, 0, s->len - s->done);
            s->done = s->len;
        } else {
            status = RICO_ST_IOERR;             /* write made no progress */
        }
    } else {
        s->done += (uint32_t)res;
        if (s->done < s->len) {                 /* short transfer: continue the tail */
            b->st.short_ios++;
            if (prep_io(b, idx, s) == 0) return 0;
            status = RICO_ST_IOERR;
        }
    }

    memset(out, 0, sizeof *out);
    out->id = s->id;
    out->tsc_submit = s->tsc_submit;
    out->status = status;
    if (status == RICO_ST_OK && s->op == RICO_OP_READ) {
        out->len = s->len;
        memcpy(out->data, slot_buf(b, idx), s->len);
    }
    if (status != RICO_ST_OK) b->st.io_errors++;

    /* release: O(1) swap-remove from the active list, slot back to the stack */
    uint16_t last = b->active[--b->nactive];
    b->active[s->active_pos] = last;
    b->slots[last].active_pos = s->active_pos;
    s->state = S_FREE;
    b->free_stack[b->nfree++] = (uint16_t)idx;
    b->st.completed++;
    return 1;
}

static unsigned be_reap(void *be, struct rico_cpl *out, unsigned max)
{
    struct rico_iouring *b = be;
    unsigned n = 0;
    struct io_uring_cqe *cqe;

    /* COOP_TASKRUN: completions needing task_work don't run until we enter the kernel */
    if (!b->sqpoll && (__atomic_load_n(b->ring.sq.kflags, __ATOMIC_RELAXED) & IORING_SQ_TASKRUN))
        io_uring_get_events(&b->ring);

    while (n < max && io_uring_peek_cqe(&b->ring, &cqe) == 0) {
        uint64_t tag = cqe->user_data;
        int res = cqe->res;
        io_uring_cqe_seen(&b->ring, cqe);
        if (tag == TAG_CANCEL) continue;

        unsigned idx = (unsigned)(tag & ((1u << SLOT_BITS) - 1));
        if (idx >= b->nslots || b->slots[idx].state != S_INFLIGHT ||
            (b->slots[idx].id & ID_MASK) != (tag >> SLOT_BITS)) {
            b->st.bad_cqes++;
            continue;
        }
        if (finish(b, idx, &b->slots[idx], res, &out[n])) n++;
    }
    /* finish() may have queued continuations (short I/O, EAGAIN/EINTR retries).
     * The worker only kicks after submitting new requests, so push them now or
     * they would sit in the SQ until some unrelated submit or wait(). */
    do_kick(b);
    return n;
}

static unsigned be_inflight(void *be) { return ((struct rico_iouring *)be)->nactive; }

static void be_wait(void *be, unsigned timeout_us)
{
    struct rico_iouring *b = be;
    struct __kernel_timespec ts = { 0, (long long)timeout_us * 1000 };
    struct io_uring_cqe *cqe;
    do_kick(b);
    io_uring_wait_cqe_timeout(&b->ring, &cqe, &ts);     /* peeks only; reap() consumes */
}

const struct rico_backend_ops rico_iouring_ops = {
    .submit = be_submit, .kick = be_kick, .reap = be_reap,
    .inflight = be_inflight, .wait = be_wait,
};

/* ---- lifecycle ------------------------------------------------------------- */

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

void rico_iouring_destroy(struct rico_iouring *b)
{
    int leak_bufs = 0;
    if (!b) return;
    if (b->ring_ok) {
        do_kick(b);
        if (b->nactive) {
            struct io_uring_sqe *sqe = get_sqe(b);
            if (sqe) {
                io_uring_prep_cancel64(sqe, 0, IORING_ASYNC_CANCEL_ANY | IORING_ASYNC_CANCEL_ALL);
                io_uring_sqe_set_data64(sqe, TAG_CANCEL);
                b->unsubmitted++;
                do_kick(b);
            }
            struct rico_cpl scratch[64];
            uint64_t deadline = now_ms() + DRAIN_TIMEOUT_MS;
            while (b->nactive && now_ms() < deadline) {
                be_reap(b, scratch, 64);
                if (b->nactive) be_wait(b, 1000);
            }
        }
        if (b->nactive) {
            fprintf(stderr, "rico_iouring: %u requests still in flight at teardown; "
                            "leaking their buffers\n", b->nactive);
            leak_bufs = 1;
        }
        io_uring_queue_exit(&b->ring);
    }
    if (b->fd >= 0) close(b->fd);
    if (!leak_bufs) free(b->bufs);
    free(b->slots); free(b->free_stack); free(b->active);
    free(b);
}

static int device_size(int fd, uint64_t *size, uint64_t min_size)
{
    struct stat st;
    if (fstat(fd, &st) < 0) return -errno;
    if (S_ISREG(st.st_mode)) {
        if (min_size && (uint64_t)st.st_size < min_size) {
            if (ftruncate(fd, (off_t)min_size) < 0) return -errno;
            st.st_size = (off_t)min_size;
        }
        *size = (uint64_t)st.st_size;
    } else if (S_ISBLK(st.st_mode)) {
        uint64_t sz;
        if (ioctl(fd, BLKGETSIZE64, &sz) < 0) return -errno;
        *size = sz;
    } else {
        return -ENODEV;
    }
    return 0;
}

struct rico_iouring *rico_iouring_create(const struct rico_iouring_cfg *cfg, int *err)
{
    int rc = 0;
    unsigned qd = cfg->qd ? cfg->qd : 256;
    struct rico_iouring *b = NULL;

    if (!cfg->path || qd > MAX_QD || (qd & (qd - 1))) { rc = -EINVAL; goto fail; }
    b = calloc(1, sizeof *b);
    if (!b) { rc = -ENOMEM; goto fail; }
    b->fd = -1;
    b->nslots = qd;
    b->slots = calloc(qd, sizeof *b->slots);
    b->free_stack = calloc(qd, sizeof *b->free_stack);
    b->active = calloc(qd, sizeof *b->active);
    b->bufs = aligned_alloc(64, ((size_t)qd * RICO_MAX_PAYLOAD + 63) & ~(size_t)63);
    if (!b->slots || !b->free_stack || !b->active || !b->bufs) { rc = -ENOMEM; goto fail; }
    memset(b->bufs, 0, (size_t)qd * RICO_MAX_PAYLOAD);
    for (unsigned i = 0; i < qd; i++) b->free_stack[i] = (uint16_t)(qd - 1 - i);
    b->nfree = qd;

    /* min_size != 0 also means "create it if missing" (regular files only) */
    b->fd = open(cfg->path, O_RDWR | O_CLOEXEC | (cfg->min_size ? O_CREAT : 0), 0600);
    if (b->fd < 0) { rc = -errno; goto fail; }
    if ((rc = device_size(b->fd, &b->dev_size, cfg->min_size))) goto fail;

    struct io_uring_params p;
    memset(&p, 0, sizeof p);
    if (cfg->sqpoll) {
        p.flags = IORING_SETUP_SQPOLL;
        p.sq_thread_idle = cfg->sqpoll_idle_ms ? cfg->sqpoll_idle_ms : 2;
        if (cfg->sqpoll_cpu >= 0) { p.flags |= IORING_SETUP_SQ_AFF; p.sq_thread_cpu = (unsigned)cfg->sqpoll_cpu; }
        b->sqpoll = 1;
    } else {
        p.flags = IORING_SETUP_COOP_TASKRUN;
    }
    rc = io_uring_queue_init_params(qd, &b->ring, &p);
    if (rc == -EINVAL && !cfg->sqpoll) {        /* kernel < 5.19: no COOP_TASKRUN */
        memset(&p, 0, sizeof p);
        rc = io_uring_queue_init_params(qd, &b->ring, &p);
    }
    if (rc < 0) goto fail;
    b->ring_ok = 1;

    if (!cfg->no_fixed) {
        b->fixed_file = io_uring_register_files(&b->ring, &b->fd, 1) == 0;
        struct iovec *iov = calloc(qd, sizeof *iov);
        if (iov) {
            for (unsigned i = 0; i < qd; i++) {
                iov[i].iov_base = slot_buf(b, i);
                iov[i].iov_len = RICO_MAX_PAYLOAD;
            }
            b->fixed_bufs = io_uring_register_buffers(&b->ring, iov, qd) == 0;
            free(iov);
        }
    }
    snprintf(b->desc, sizeof b->desc, "qd=%u %s%s%s dev=%llu bytes", qd,
             b->sqpoll ? "sqpoll " : "", b->fixed_file ? "fixed-file " : "",
             b->fixed_bufs ? "fixed-bufs" : "", (unsigned long long)b->dev_size);
    if (err) *err = 0;
    return b;
fail:
    rico_iouring_destroy(b);
    if (err) *err = rc;
    return NULL;
}

uint64_t rico_iouring_size(const struct rico_iouring *b) { return b->dev_size; }
const struct rico_iouring_stats *rico_iouring_get_stats(const struct rico_iouring *b) { return &b->st; }
const char *rico_iouring_describe(const struct rico_iouring *b) { return b->desc; }

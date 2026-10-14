/* rico_host.c - host-side worker: map the region, poll the ring on a thread. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "rico_host.h"
#include "rico_pin.h"

int rico_host_create(const char *path)
{
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    if (ftruncate(fd, RICO_SHM_SIZE) < 0) { close(fd); return -1; }
    struct rico_shm *s = mmap(NULL, RICO_SHM_SIZE, PROT_READ | PROT_WRITE,
                              MAP_SHARED, fd, 0);
    close(fd);
    if (s == MAP_FAILED) return -1;
    memset(s, 0, sizeof *s);                /* O_TRUNC already zeroed; be explicit */
    s->hdr.magic    = RICO_SHM_MAGIC;
    s->hdr.version  = RICO_SHM_VER;
    s->hdr.slots    = RICO_RING_SLOTS;
    s->hdr.req_size = sizeof(struct rico_req);
    s->hdr.cq_slots = RICO_CQ_SLOTS;
    s->hdr.cpl_size = sizeof(struct rico_cpl);
    atomic_store_explicit(&s->hdr.host_ready, 1, memory_order_release);
    munmap(s, RICO_SHM_SIZE);
    return 0;
}

static const struct rico_backend_ops sync_ops;   /* defined below */

int rico_host_open(struct rico_host *h, const char *path,
                   rico_handler_fn fn, void *opaque, int cpu)
{
    memset(h, 0, sizeof *h);
    h->fd = open(path, O_RDWR);
    if (h->fd < 0) return -1;
    struct stat st;
    if (fstat(h->fd, &st) < 0 || (size_t)st.st_size < RICO_SHM_SIZE) {
        errno = EINVAL; goto fail;
    }
    h->map_len = RICO_SHM_SIZE;
    h->shm = mmap(NULL, h->map_len, PROT_READ | PROT_WRITE, MAP_SHARED, h->fd, 0);
    if (h->shm == MAP_FAILED) goto fail;
    if (h->shm->hdr.magic != RICO_SHM_MAGIC || h->shm->hdr.version != RICO_SHM_VER ||
        h->shm->hdr.slots != RICO_RING_SLOTS ||
        h->shm->hdr.req_size != sizeof(struct rico_req) ||
        h->shm->hdr.cq_slots != RICO_CQ_SLOTS ||
        h->shm->hdr.cpl_size != sizeof(struct rico_cpl)) {
        errno = EPROTO; munmap(h->shm, h->map_len); goto fail;
    }
    h->handler = fn; h->opaque = opaque; h->cpu = cpu;
    h->ops = &sync_ops; h->be = h;
    return 0;
fail:
    close(h->fd);
    return -1;
}

/* ---- built-in synchronous backend: wraps the handler callback ----------- */

static int sync_submit(void *be, const struct rico_req *q, struct rico_cpl *c)
{
    struct rico_host *h = be;
    h->handler(q, c, h->opaque);
    return RICO_BE_DONE;
}
static const struct rico_backend_ops sync_ops = { .submit = sync_submit };

void rico_host_set_backend(struct rico_host *h, const struct rico_backend_ops *ops, void *be)
{
    h->ops = ops;
    h->be = be;
}

/* ---- worker loop ---------------------------------------------------------- */

#define RICO_HOST_BATCH   32u       /* max SQ entries ingested between kicks */
#define RICO_EGRESS_CAP   1024u     /* power of two: completions staged for the CQ */
#define RICO_REAP_MAX     64u
#define RICO_SPIN_POLLS   4096u     /* idle polls before backing off */
#define RICO_IDLE_US      50u

static inline void idle_sleep(void)
{
    struct timespec ts = { 0, RICO_IDLE_US * 1000 };
    nanosleep(&ts, NULL);
}

/*
 * One iteration, in this order, so completions leave as early as possible and
 * the kernel gets work in batches:
 *
 *   1. egress : push staged completions to the CQ (never blocks)
 *   2. ingest : pop up to BATCH requests from the SQ -> backend.submit()
 *               then ONE backend.kick() for the whole batch
 *   3. reap   : backend.reap() -> stage completions, push them straight away
 *   4. idle   : spin, then wait in the backend (if work is in flight) or sleep
 *
 * The worker never blocks on a full CQ (that would stop it reaping): finished
 * requests wait in a bounded egress FIFO, and ingest is gated on
 *     egress_n + backend_inflight < EGRESS_CAP
 * so staging can't overflow. A guest that stops draining the CQ therefore
 * throttles itself: egress fills -> SQ pops stop -> SQ fills -> the guest sees a
 * full SQ and (by design) drains the CQ while it spins.
 *
 * The guest is untrusted: every request is copied once by rico_ring_pop(), then
 * op/len are validated before any backend sees it. Invalid requests still get a
 * completion (RICO_ST_INVAL) so the guest never waits on them.
 */
void *rico_host_run(void *arg)
{
    struct rico_host *h = arg;
    struct rico_ring *sq = &h->shm->sq;
    const struct rico_backend_ops *ops = h->ops;
    void *be = h->be;
    struct rico_cpl *eg = calloc(RICO_EGRESS_CAP, sizeof *eg);
    struct rico_cpl out[RICO_REAP_MAX];
    struct rico_req pend;               /* request the backend answered BUSY to */
    unsigned eg_head = 0, eg_n = 0, idle = 0;
    int have_pend = 0;

    if (!eg) return NULL;
    rico_pin_self(h->cpu);

#define EG_ADD(cp) do { eg[(eg_head + eg_n++) & (RICO_EGRESS_CAP - 1)] = *(cp); } while (0)

    while (!atomic_load_explicit(&h->stop, memory_order_relaxed)) {
        int progress = 0;
        unsigned pending_submits = 0;

        /* 1. egress */
        while (eg_n) {
            struct rico_cpl c = eg[eg_head];
            c.tsc_complete = rico_rdtsc();      /* host clock; stamped last */
            if (!rico_cq_push(&h->shm->cq, &c)) break;
            eg_head = (eg_head + 1) & (RICO_EGRESS_CAP - 1);
            eg_n--; h->completed++; progress = 1;
        }

        /* 2. ingest */
        for (unsigned budget = RICO_HOST_BATCH; budget; ) {
            unsigned inflight = ops->inflight ? ops->inflight(be) : 0;
            if (eg_n + inflight >= RICO_EGRESS_CAP) break;      /* backpressure */

            struct rico_req q;
            struct rico_cpl c;
            int r;
            if (have_pend) {
                q = pend;
            } else {
                if (!rico_ring_pop(sq, &q)) {
                    if (rico_ring_corrupt(sq)) {    /* guest scribbled on tail: stop */
                        h->ring_corrupt = 1;
                        goto done;
                    }
                    break;
                }
                h->consumed++;
            }
            memset(&c, 0, sizeof c);
            c.id = q.id;
            c.tsc_submit = q.tsc_submit;            /* echo, untouched */
            if (!have_pend &&
                ((q.op != RICO_OP_READ && q.op != RICO_OP_WRITE && q.op != RICO_OP_FLUSH) ||
                 q.len > RICO_MAX_PAYLOAD)) {
                h->bad_requests++;
                c.status = RICO_ST_INVAL;
                r = RICO_BE_DONE;
            } else {
                r = ops->submit(be, &q, &c);
            }
            if (r == RICO_BE_BUSY) { pend = q; have_pend = 1; break; }
            have_pend = 0; progress = 1; budget--;
            if (r == RICO_BE_DONE) EG_ADD(&c); else pending_submits++;
        }
        if ((pending_submits || have_pend) && ops->kick) ops->kick(be);

        /* 3. reap, then push right away (don't wait a whole loop turn) */
        if (ops->reap) {
            unsigned n = ops->reap(be, out, RICO_REAP_MAX);
            for (unsigned i = 0; i < n; i++) EG_ADD(&out[i]);
            if (n) progress = 1;
            while (eg_n) {
                struct rico_cpl c = eg[eg_head];
                c.tsc_complete = rico_rdtsc();
                if (!rico_cq_push(&h->shm->cq, &c)) break;
                eg_head = (eg_head + 1) & (RICO_EGRESS_CAP - 1);
                eg_n--; h->completed++; progress = 1;
            }
        }

        /* 4. idle policy */
        if (progress) { idle = 0; continue; }
        if (h->busy_poll || ++idle < RICO_SPIN_POLLS) {
            rico_cpu_relax();
        } else if (!eg_n && ops->inflight && ops->inflight(be) && ops->wait) {
            ops->wait(be, RICO_IDLE_US);        /* sleeps in the kernel until a CQE */
        } else {
            idle_sleep();
        }
    }
done:
    free(eg);
    return NULL;
}

int rico_host_start(struct rico_host *h)
{
    if (pthread_create(&h->thread, NULL, rico_host_run, h)) return -1;
    h->started = 1;
    return 0;
}

void rico_host_stop(struct rico_host *h)
{
    atomic_store_explicit(&h->stop, 1, memory_order_relaxed);
    if (h->started) { pthread_join(h->thread, NULL); h->started = 0; }
}

void rico_host_close(struct rico_host *h)
{
    munmap(h->shm, h->map_len);
    close(h->fd);
}

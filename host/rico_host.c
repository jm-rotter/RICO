/* rico_host.c - host-side worker: map the region, poll the ring on a thread. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
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
    return 0;
fail:
    close(h->fd);
    return -1;
}

/*
 * Push one completion. If the CQ is full the guest isn't draining; wait for it
 * (with backoff) but stay responsive to stop. Returns 0 if pushed.
 */
static int cq_push_wait(struct rico_host *h, const struct rico_cpl *c)
{
    unsigned spins = 0;
    while (!rico_cq_push(&h->shm->cq, c)) {
        if (atomic_load_explicit(&h->stop, memory_order_relaxed)) return -1;
        if (++spins < 4096) {
            rico_cpu_relax();
        } else {
            struct timespec ts = { 0, 50 * 1000 };
            nanosleep(&ts, NULL);
        }
    }
    h->completed++;
    return 0;
}

/*
 * Polling loop. The guest is untrusted: every request is copied once by
 * rico_ring_pop(), then op/len are validated before the handler sees it. An
 * invalid request still gets a completion (RICO_ST_INVAL) so the guest never
 * waits forever on it.
 * Idle strategy: spin briefly (low latency under load), then back off with
 * short sleeps so an idle VM doesn't pin a host core at 100%.
 */
void *rico_host_run(void *arg)
{
    struct rico_host *h = arg;
    struct rico_ring *sq = &h->shm->sq;
    struct rico_req q;
    struct rico_cpl c;
    unsigned idle = 0;

    rico_pin_self(h->cpu);

    while (!atomic_load_explicit(&h->stop, memory_order_relaxed)) {
        if (!rico_ring_pop(sq, &q)) {
            if (rico_ring_corrupt(sq)) {    /* guest scribbled on tail: stop */
                h->ring_corrupt = 1;
                break;
            }
            if (h->busy_poll || ++idle < 4096) {
                rico_cpu_relax();
            } else {
                struct timespec ts = { 0, 50 * 1000 };      /* 50 us */
                nanosleep(&ts, NULL);
            }
            continue;
        }
        idle = 0;
        h->consumed++;
        memset(&c, 0, sizeof c);
        c.id = q.id;
        c.tsc_submit = q.tsc_submit;        /* echo, untouched */
        if ((q.op != RICO_OP_READ && q.op != RICO_OP_WRITE && q.op != RICO_OP_FLUSH) ||
            q.len > RICO_MAX_PAYLOAD) {
            h->bad_requests++;
            c.status = RICO_ST_INVAL;
        } else {
            h->handler(&q, &c, h->opaque);
        }
        c.tsc_complete = rico_rdtsc();      /* host clock; stamped last */
        if (cq_push_wait(h, &c)) break;
    }
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

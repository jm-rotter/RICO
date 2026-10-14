/*
 * rico_guest.h - guest-side submit/complete loop. Header-only so the guest
 * binary and the in-process test share it.
 *
 * Two regimes, selected by cfg.depth (max requests in flight):
 *   depth == 0 (or > ring size): LOADED. The guest keeps the SQ as full as it
 *       can. Gives peak throughput; per-request latency includes time queued
 *       behind earlier requests, so it is NOT the cost of crossing the channel.
 *   depth == 1: PING-PONG. Push one request, wait for its completion, only
 *       then push the next. The channel is empty at every push, so the
 *       round-trip is the true unloaded latency.
 *
 * The loop never blocks without draining the CQ. That matters: the host stalls
 * when the CQ is full, so a guest that spun on a full SQ without consuming
 * completions would deadlock both sides.
 *
 * Verification: completions are matched by id and may arrive out of order (an
 * async backend finishes non-overlapping requests in any order). The host
 * guarantees that overlapping requests (>= 1 write) execute, and so complete,
 * in submission order, so replaying writes into a model device at completion
 * time and checking each READ against it is exact. Completions come from the
 * (untrusted) host: an id that isn't in flight (forged, duplicated, stale) is
 * counted as an error and never as progress.
 */
#ifndef RICO_GUEST_H
#define RICO_GUEST_H

#include <stdlib.h>
#include "rico_lat.h"
#include "rico_pin.h"
#include "rico_shm.h"

/* Ids in flight are tracked in a table this big, so the guest never has more
 * than RICO_GUEST_MAX_INFLIGHT outstanding (>= SQ + CQ + host staging). */
#define RICO_GUEST_MAX_INFLIGHT 4096u
#define RICO_GUEST_IDTAB        8192u
#define RICO_NO_ID              (~0ull)

struct rico_guest_cfg {
    uint64_t n;                             /* total requests, incl. warmup */
    uint64_t depth;                         /* max in flight; 0 = unbounded (loaded) */
    uint64_t warmup;                        /* leading completions excluded from stats */
    int      cq_leg;                        /* record host->guest leg: only valid if
                                             * guest and host share a TSC domain */
    int      cpu;                           /* pin to this CPU; -1 = don't */
};

struct rico_guest_result {
    uint64_t submitted, completed, errors;
    double   seconds;                       /* wall time of the measured loop */
    struct rico_lat rtt;                    /* pop_tsc - tsc_submit (guest clock only) */
    struct rico_lat cq_leg;                 /* pop_tsc - host tsc_complete */
};

/* Returns number of verification errors (0 = pass). Fills res; the caller
 * must pass res->rtt and res->cq_leg through rico_lat_report() (or free). */
static inline uint64_t rico_guest_run(struct rico_shm *s, const struct rico_guest_cfg *cfg,
                                      struct rico_guest_result *res)
{
    uint64_t n = cfg->n;
    uint64_t depth = cfg->depth && cfg->depth < RICO_GUEST_MAX_INFLIGHT ? cfg->depth
                                                                          : RICO_GUEST_MAX_INFLIGHT;
    memset(res, 0, sizeof *res);
    uint8_t *model = calloc(1, RICO_DEV_BYTES);
    uint64_t *inflight_id = malloc(RICO_GUEST_IDTAB * sizeof *inflight_id);
    if (!model || !inflight_id || rico_lat_init(&res->rtt, n) || rico_lat_init(&res->cq_leg, n))
        return ~0ull;
    for (unsigned i = 0; i < RICO_GUEST_IDTAB; i++) inflight_id[i] = RICO_NO_ID;
    rico_pin_self(cfg->cpu);

    uint64_t submitted = 0, completed = 0, errors = 0;
    struct rico_req q, e;
    struct rico_cpl c;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (completed < n) {
        int progress = 0;

        if (submitted < n && submitted - completed < depth) {
            rico_make_req(submitted, &q);
            q.tsc_submit = rico_rdtsc();    /* as late as possible: right before the push */
            if (rico_ring_push(&s->sq, &q)) {
                inflight_id[q.id & (RICO_GUEST_IDTAB - 1)] = q.id;
                submitted++; progress = 1;
            }
        }

        while (rico_cq_pop(&s->cq, &c)) {   /* drain everything available */
            uint64_t tsc_pop = rico_rdtsc();    /* right after the pop */
            progress = 1;
            if (c.id >= submitted || inflight_id[c.id & (RICO_GUEST_IDTAB - 1)] != c.id) {
                errors++;                   /* forged / duplicate / stale id: not a completion */
                if (rico_cq_corrupt(&s->cq)) goto out;
                continue;
            }
            inflight_id[c.id & (RICO_GUEST_IDTAB - 1)] = RICO_NO_ID;
            if (completed >= cfg->warmup) {
                rico_lat_add(&res->rtt, tsc_pop - c.tsc_submit);
                if (cfg->cq_leg) rico_lat_add(&res->cq_leg, tsc_pop - c.tsc_complete);
            }
            rico_make_req(c.id, &e);
            if (c.status != RICO_ST_OK || c.len > RICO_MAX_CPL_DATA) {
                errors++;
            } else if (e.op == RICO_OP_READ) {
                if (c.len != e.len || memcmp(c.data, &model[e.offset], e.len))
                    errors++;
            } else if (c.len != 0) {
                errors++;
            }
            if (e.op == RICO_OP_WRITE)
                memcpy(&model[e.offset], e.payload, e.len);
            if (++completed == n) break;
            if (rico_cq_corrupt(&s->cq)) { errors++; goto out; }
        }
        if (rico_cq_corrupt(&s->cq)) { errors++; break; }
        if (!progress) rico_cpu_relax();
    }
out:
    clock_gettime(CLOCK_MONOTONIC, &t1);
    res->seconds = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
    free(model); free(inflight_id);
    res->submitted = submitted; res->completed = completed; res->errors = errors;
    return errors;
}

/*
 * Report, keeping the two regimes visibly apart: loaded runs headline
 * throughput and flag latency as queue-inclusive; ping-pong runs headline
 * latency and omit throughput (it is just 1/RTT and says nothing new).
 */
static inline void rico_guest_report(const struct rico_guest_cfg *cfg,
                                     struct rico_guest_result *r, double hz)
{
    if (cfg->depth == 1) {
        printf("UNLOADED ping-pong (depth 1, %llu warmup discarded)\n",
               (unsigned long long)cfg->warmup);
        rico_lat_report("round-trip latency", &r->rtt, hz);
        if (cfg->cq_leg) rico_lat_report("host->guest (CQ) leg", &r->cq_leg, hz);
    } else {
        printf("LOADED (depth %s%llu): %.2f Mreq/s over %.3f s\n",
               cfg->depth ? "" : "unbounded/", (unsigned long long)cfg->depth,
               r->completed / r->seconds / 1e6, r->seconds);
        printf("  NOTE: latencies below include queueing; not the unloaded cost\n");
        rico_lat_report("round-trip incl. queueing", &r->rtt, hz);
        if (cfg->cq_leg) rico_lat_report("host->guest (CQ) leg", &r->cq_leg, hz);
    }
}

#endif

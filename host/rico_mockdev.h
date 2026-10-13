/*
 * rico_mockdev.h - host-side mock block device behind the ring. Header-only so
 * the daemon and the test share it.
 */
#ifndef RICO_MOCKDEV_H
#define RICO_MOCKDEV_H

#include "rico_shm.h"

struct rico_mockdev {
    uint8_t  dev[RICO_DEV_BYTES];           /* host-private, never shared */
    uint64_t next_id;
    uint64_t errors;                        /* workload/order mismatches seen */
};

/* rico_handler_fn: fill cpl (already zeroed, id set) for one request. */
static inline void rico_mockdev_handle(const struct rico_req *q,
                                       struct rico_cpl *cpl, void *opaque)
{
    struct rico_mockdev *d = opaque;
    struct rico_req expect;
    rico_make_req(q->id, &expect);
    expect.tsc_submit = q->tsc_submit;      /* guest-stamped; not part of the workload */
    if (q->id != d->next_id || memcmp(q, &expect, sizeof *q)) d->errors++;
    d->next_id++;

    if (q->op != RICO_OP_FLUSH && q->offset > RICO_DEV_BYTES - q->len) {
        cpl->status = RICO_ST_INVAL;        /* guest-supplied offset: check */
        return;
    }
    switch (q->op) {
    case RICO_OP_WRITE:
        memcpy(&d->dev[q->offset], q->payload, q->len);
        break;
    case RICO_OP_READ:
        if (q->len > RICO_MAX_CPL_DATA) { cpl->status = RICO_ST_INVAL; return; }
        memcpy(cpl->data, &d->dev[q->offset], q->len);
        cpl->len = q->len;
        break;
    default:                                /* FLUSH: nothing to do in a mock */
        break;
    }
    cpl->status = RICO_ST_OK;
}

#endif

/*
 * rico_iouring_test.c - unit tests for the io_uring backend, driven directly
 * through its ops table (no rings, no guest). Built with AddressSanitizer in
 * `make test`, so teardown with I/O in flight is checked for use-after-free.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rico_iouring.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define DEV 4096u

static struct rico_req mk(uint64_t id, unsigned op, uint64_t off, unsigned len, uint8_t fill)
{
    struct rico_req q;
    memset(&q, 0, sizeof q);
    q.id = id; q.op = op; q.offset = off; q.len = len;
    memset(q.payload, fill, sizeof q.payload);
    return q;
}

static int submit(struct rico_iouring *b, const struct rico_req *q, struct rico_cpl *c)
{
    memset(c, 0, sizeof *c);
    c->id = q->id; c->tsc_submit = q->tsc_submit;
    return rico_iouring_ops.submit(b, q, c);
}

/* Kick + reap until `want` completions have been collected (or time runs out). */
static unsigned pump(struct rico_iouring *b, struct rico_cpl *out, unsigned want)
{
    unsigned n = 0;
    for (int spin = 0; n < want && spin < 2000000; spin++) {
        rico_iouring_ops.kick(b);
        n += rico_iouring_ops.reap(b, out + n, want - n);
    }
    return n;
}

static struct rico_iouring *mkbe(const char *path, unsigned qd)
{
    struct rico_iouring_cfg cfg = { .path = path, .min_size = DEV, .qd = qd, .sqpoll_cpu = -1 };
    int err = 0;
    struct rico_iouring *b = rico_iouring_create(&cfg, &err);
    if (!b) printf("create failed: %s\n", strerror(-err));
    return b;
}

int main(void)
{
    char path[64];
    snprintf(path, sizeof path, "/dev/shm/rico_iouring_test.%d", getpid());
    struct rico_cpl c, out[8];
    struct rico_req q;

    /* --- bounds: guest-controlled offset/len must never reach the kernel ---- */
    struct rico_iouring *b = mkbe(path, 8);
    if (!b) return 2;
    CHECK(rico_iouring_size(b) == DEV);
    q = mk(1, RICO_OP_READ, DEV - 1, 2, 0);          /* crosses the end */
    CHECK(submit(b, &q, &c) == RICO_BE_DONE && c.status == RICO_ST_INVAL);
    q = mk(2, RICO_OP_WRITE, ~0ull, 1, 0);           /* offset + len would overflow */
    CHECK(submit(b, &q, &c) == RICO_BE_DONE && c.status == RICO_ST_INVAL);
    q = mk(3, RICO_OP_READ, ~0ull - 3, 32, 0);
    CHECK(submit(b, &q, &c) == RICO_BE_DONE && c.status == RICO_ST_INVAL);
    q = mk(4, RICO_OP_READ, DEV - 32, 32, 0);        /* exactly fits */
    CHECK(submit(b, &q, &c) == RICO_BE_PENDING);
    CHECK(pump(b, out, 1) == 1 && out[0].id == 4 && out[0].status == RICO_ST_OK && out[0].len == 32);
    CHECK(rico_iouring_get_stats(b)->range_rejects == 3);

    /* --- write then overlapping read: must observe the write ----------------- */
    q = mk(10, RICO_OP_WRITE, 100, 16, 0xAB);
    CHECK(submit(b, &q, &c) == RICO_BE_PENDING);
    q = mk(11, RICO_OP_READ, 108, 16, 0);            /* overlaps [108,116) */
    CHECK(submit(b, &q, &c) == RICO_BE_BUSY);        /* held until the write is done */
    CHECK(pump(b, out, 1) == 1 && out[0].id == 10 && out[0].status == RICO_ST_OK);
    CHECK(submit(b, &q, &c) == RICO_BE_PENDING);     /* same request, retried */
    CHECK(pump(b, out, 1) == 1 && out[0].id == 11 && out[0].len == 16);
    CHECK(out[0].data[0] == 0xAB && out[0].data[7] == 0xAB && out[0].data[8] == 0);

    /* --- disjoint requests and read/read overlap run concurrently ------------ */
    q = mk(20, RICO_OP_READ, 0, 8, 0);
    CHECK(submit(b, &q, &c) == RICO_BE_PENDING);
    q = mk(21, RICO_OP_READ, 4, 8, 0);               /* read/read overlap: allowed */
    CHECK(submit(b, &q, &c) == RICO_BE_PENDING);
    q = mk(22, RICO_OP_WRITE, 2000, 8, 1);           /* disjoint write: allowed */
    CHECK(submit(b, &q, &c) == RICO_BE_PENDING);
    q = mk(23, RICO_OP_WRITE, 6, 4, 2);              /* overlaps both reads: held */
    CHECK(submit(b, &q, &c) == RICO_BE_BUSY);
    CHECK(rico_iouring_ops.inflight(b) == 3);
    CHECK(pump(b, out, 3) == 3);

    /* --- FLUSH is a barrier: waits for the pipeline to drain ------------------ */
    q = mk(30, RICO_OP_WRITE, 0, 4, 3);
    CHECK(submit(b, &q, &c) == RICO_BE_PENDING);
    q = mk(31, RICO_OP_FLUSH, 0, 0, 0);
    CHECK(submit(b, &q, &c) == RICO_BE_BUSY);
    CHECK(pump(b, out, 1) == 1);
    CHECK(submit(b, &q, &c) == RICO_BE_PENDING);
    CHECK(pump(b, out, 1) == 1 && out[0].id == 31 && out[0].status == RICO_ST_OK && out[0].len == 0);

    /* --- zero-length requests complete immediately ----------------------------- */
    q = mk(40, RICO_OP_WRITE, 5, 0, 0);
    CHECK(submit(b, &q, &c) == RICO_BE_DONE && c.status == RICO_ST_OK);
    CHECK(rico_iouring_get_stats(b)->io_errors == 0 && rico_iouring_get_stats(b)->bad_cqes == 0);
    rico_iouring_destroy(b);

    /* --- slot exhaustion: qd=2 --------------------------------------------------- */
    b = mkbe(path, 2);
    q = mk(50, RICO_OP_READ, 0, 4, 0);
    CHECK(submit(b, &q, &c) == RICO_BE_PENDING);
    q = mk(51, RICO_OP_READ, 8, 4, 0);
    CHECK(submit(b, &q, &c) == RICO_BE_PENDING);
    q = mk(52, RICO_OP_READ, 16, 4, 0);
    CHECK(submit(b, &q, &c) == RICO_BE_BUSY);
    CHECK(rico_iouring_get_stats(b)->slot_stalls == 1);
    CHECK(pump(b, out, 2) == 2);
    CHECK(submit(b, &q, &c) == RICO_BE_PENDING);     /* slots recycled */
    CHECK(pump(b, out, 1) == 1);
    rico_iouring_destroy(b);

    /* --- file shrunk underneath us: read past EOF returns zeros, not an error --- */
    b = mkbe(path, 4);
    q = mk(60, RICO_OP_WRITE, 0, 8, 0x77);
    CHECK(submit(b, &q, &c) == RICO_BE_PENDING && pump(b, out, 1) == 1);
    CHECK(truncate(path, 0) == 0);
    q = mk(61, RICO_OP_READ, 0, 8, 0);
    CHECK(submit(b, &q, &c) == RICO_BE_PENDING);
    CHECK(pump(b, out, 1) == 1 && out[0].status == RICO_ST_OK && out[0].len == 8 &&
          out[0].data[0] == 0 && out[0].data[7] == 0);
    rico_iouring_destroy(b);

    /* --- teardown with I/O in flight (ASan checks buffers outlive the kernel) --- */
    b = mkbe(path, 64);
    for (unsigned i = 0; i < 32; i++) {
        q = mk(100 + i, i & 1 ? RICO_OP_READ : RICO_OP_WRITE, i * 64, 32, 5);
        CHECK(submit(b, &q, &c) == RICO_BE_PENDING);
    }
    rico_iouring_destroy(b);                         /* no reap: destroy must drain/cancel */

    /* --- bad configuration is rejected -------------------------------------------- */
    struct rico_iouring_cfg bad = { .path = path, .qd = 3 };    /* not a power of two */
    int err = 0;
    CHECK(rico_iouring_create(&bad, &err) == NULL && err == -EINVAL);
    struct rico_iouring_cfg missing = { .path = "/nonexistent/rico_img", .qd = 4 };
    CHECK(rico_iouring_create(&missing, &err) == NULL && err < 0);

    unlink(path);
    printf(failures ? "rico_iouring_test: %d FAILURES\n" : "rico_iouring_test: all passed\n", failures);
    return failures != 0;
}

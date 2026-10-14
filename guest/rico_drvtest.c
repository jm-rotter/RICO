/*
 * rico_drvtest.c - smoke/integration test for the rico.ko character device.
 *
 *   rico_drvtest [/dev/rico] <n> [--depth D]
 *
 * Sends EXACTLY n requests to the host in total, so rico_hostd can be started
 * with the same n and exit on its own:
 *
 *   1. argument/ioctl/errno checks            (rejected in-kernel, no host traffic)
 *   2. backpressure: fill all 1024 slots with FLUSHes, check partial write,
 *      EAGAIN instead of a hang, POLLOUT clear, then drain them
 *   3. orphans: 100 FLUSHes on an fd that is closed without reading
 *   4. main phase: n - 1124 requests of the deterministic workload through
 *      write/poll/RICO_IOC_WAIT/read, with every READ verified against a
 *      model device (same exact-replay argument as guest/rico_guest.h)
 *   5. device stats: submitted == completed == n, no bad completions
 *
 * Exit status 0 = pass. Needs n >= 2000.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "rico_shm.h"       /* rico_make_req (host/guest shared workload) */
#include "rico_uapi.h"      /* must come after rico_shm.h: see its #ifndefs */

#define BP_COUNT   1024u    /* == driver slot capacity */
#define ORPHANS    100u
#define BATCH      32

static unsigned long errors;

#define CHECK(cond, ...) do {                                           \
    if (!(cond)) {                                                      \
        errors++;                                                       \
        fprintf(stderr, "drvtest: FAIL line %d: ", __LINE__);           \
        fprintf(stderr, __VA_ARGS__);                                   \
        fputc('\n', stderr);                                            \
    }                                                                   \
} while (0)

static void on_alarm(int s) { (void)s; static const char m[] = "drvtest: TIMEOUT (hang)\n"; if (write(2, m, sizeof m - 1)) {} _exit(3); }

static int open_dev(const char *path, int flags)
{
    int fd = open(path, O_RDWR | flags);
    if (fd < 0) { perror("drvtest: open"); exit(1); }
    return fd;
}

static void make_flush(struct rico_sqe *s, uint64_t cookie)
{
    memset(s, 0, sizeof *s);
    s->user_data = cookie;
    s->op = RICO_OP_FLUSH;
}

static void sqe_from_req(struct rico_sqe *s, uint64_t id)
{
    struct rico_req q;
    rico_make_req(id, &q);
    memset(s, 0, sizeof *s);
    s->user_data = id;
    s->op = q.op;
    s->len = q.len;
    s->offset = q.offset;
    memcpy(s->payload, q.payload, q.len);
}

/* Expect write(fd, &s, sizeof s) to fail with `err`. */
static void expect_bad_write(int fd, const void *buf, size_t len, int err, const char *what)
{
    errno = 0;
    ssize_t r = write(fd, buf, len);
    CHECK(r == -1 && errno == err, "%s: write returned %zd errno %d (want -1/%d)", what, r, errno, err);
}

static void test_errors(const char *path)
{
    struct rico_sqe s;
    struct rico_cqe c[2];
    struct rico_info info;
    struct rico_wait w;
    int fd = open_dev(path, 0), nb = open_dev(path, O_NONBLOCK);

    CHECK(ioctl(fd, RICO_IOC_GETINFO, &info) == 0, "GETINFO: %s", strerror(errno));
    CHECK(info.abi_version == RICO_ABI_VERSION, "abi %u", info.abi_version);
    CHECK(info.sq_slots == RICO_RING_SLOTS && info.cq_slots == RICO_CQ_SLOTS, "ring sizes");
    CHECK(info.max_inflight == BP_COUNT, "max_inflight %u", info.max_inflight);
    CHECK(info.shm_size == RICO_SHM_SIZE, "shm_size %llu", (unsigned long long)info.shm_size);
    CHECK(info.submitted == 0 && info.completed == 0 && info.bad_cpl == 0, "stats not zero at start");

    expect_bad_write(fd, &s, 1, EINVAL, "short record");
    make_flush(&s, 1); s.op = 0;           expect_bad_write(fd, &s, sizeof s, EINVAL, "op 0");
    make_flush(&s, 1); s.op = 9;           expect_bad_write(fd, &s, sizeof s, EINVAL, "op 9");
    make_flush(&s, 1); s.len = 1;          expect_bad_write(fd, &s, sizeof s, EINVAL, "flush with len");
    make_flush(&s, 1); s.op = RICO_OP_READ; s.len = 0;  expect_bad_write(fd, &s, sizeof s, EINVAL, "read len 0");
    make_flush(&s, 1); s.op = RICO_OP_READ; s.len = 33; expect_bad_write(fd, &s, sizeof s, EINVAL, "read len 33");
    make_flush(&s, 1); s.reserved = 1;     expect_bad_write(fd, &s, sizeof s, EINVAL, "reserved");

    errno = 0;
    CHECK(read(fd, c, sizeof(c[0]) - 1) == -1 && errno == EINVAL, "short read buf: errno %d", errno);
    errno = 0;
    CHECK(read(nb, c, sizeof c) == -1 && errno == EAGAIN, "nonblock empty read: errno %d", errno);
    errno = 0;
    CHECK(read(fd, c, sizeof c) == -1 && errno == ENODATA, "blocking read, nothing in flight: errno %d", errno);

    memset(&w, 0, sizeof w);
    errno = 0;
    CHECK(ioctl(fd, RICO_IOC_WAIT, &w) == -1 && errno == EINVAL, "WAIT min 0: errno %d", errno);
    w.min_complete = 1;
    errno = 0;
    CHECK(ioctl(nb, RICO_IOC_WAIT, &w) == -1 && errno == EAGAIN, "nonblock WAIT: errno %d", errno);
    errno = 0;
    CHECK(ioctl(fd, RICO_IOC_WAIT, &w) == -1 && errno == ENODATA, "WAIT nothing in flight: errno %d", errno);
    w.flags = 1;
    errno = 0;
    CHECK(ioctl(fd, RICO_IOC_WAIT, &w) == -1 && errno == EINVAL, "WAIT flags: errno %d", errno);
    errno = 0;
    CHECK(ioctl(fd, _IO(RICO_IOC_MAGIC, 0x7f), 0) == -1 && errno == ENOTTY, "bad ioctl: errno %d", errno);
    errno = 0;
    CHECK(lseek(fd, 0, SEEK_SET) == -1 && errno == ESPIPE, "lseek: errno %d", errno);

    /* None of the above may have produced host traffic. */
    CHECK(ioctl(fd, RICO_IOC_GETINFO, &info) == 0 && info.submitted == 0, "rejected requests reached the SQ");
    close(nb); close(fd);
    puts("drvtest: errno/ioctl checks done");
}

static void test_backpressure(const char *path)
{
    static struct rico_sqe sqe[BP_COUNT + 476];
    static struct rico_cqe cqe[64];
    static unsigned char seen[BP_COUNT];
    const uint64_t base = 0xb000000000ull;
    int fd = open_dev(path, 0);
    struct pollfd p = { .fd = fd, .events = POLLOUT };
    size_t total = sizeof sqe / sizeof sqe[0], got = 0;
    ssize_t r;

    for (size_t i = 0; i < total; i++) make_flush(&sqe[i], base + i);

    r = write(fd, sqe, sizeof sqe);          /* more than capacity: must be partial */
    CHECK(r == (ssize_t)(BP_COUNT * sizeof sqe[0]), "fill write returned %zd (want %zu)",
          r, (size_t)BP_COUNT * sizeof sqe[0]);

    errno = 0;                               /* blocking fd, all slots held: EAGAIN, not a hang */
    r = write(fd, &sqe[0], sizeof sqe[0]);
    CHECK(r == -1 && errno == EAGAIN, "write when full: %zd errno %d", r, errno);
    CHECK(poll(&p, 1, 0) >= 0 && !(p.revents & POLLOUT), "POLLOUT set while full");

    while (got < BP_COUNT) {                 /* drain: blocking reads, any batch size */
        r = read(fd, cqe, sizeof cqe);
        if (r < 0) { CHECK(0, "drain read: %s", strerror(errno)); break; }
        for (size_t i = 0; i < (size_t)r / sizeof cqe[0]; i++, got++) {
            uint64_t k = cqe[i].user_data - base;
            CHECK(k < BP_COUNT && !seen[k], "bad/duplicate cookie %llx", (unsigned long long)cqe[i].user_data);
            if (k < BP_COUNT) seen[k] = 1;
            CHECK(cqe[i].status == RICO_ST_OK && cqe[i].len == 0, "flush cqe status %u len %u",
                  cqe[i].status, cqe[i].len);
            CHECK(cqe[i].tsc_submit != 0, "tsc_submit not stamped");
        }
    }
    CHECK(got == BP_COUNT, "drained %zu of %u", got, BP_COUNT);
    p.revents = 0;
    CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLOUT), "POLLOUT clear after drain");
    close(fd);
    puts("drvtest: backpressure checks done");
}

static void test_orphans(const char *path)
{
    struct rico_sqe sqe[ORPHANS];
    int fd = open_dev(path, 0);
    for (unsigned i = 0; i < ORPHANS; i++) make_flush(&sqe[i], 0xdead0000u + i);
    ssize_t r = write(fd, sqe, sizeof sqe);
    CHECK(r == (ssize_t)sizeof sqe, "orphan write returned %zd", r);
    close(fd);                               /* in flight or done, never read */
    puts("drvtest: orphan requests abandoned");
}

static void wait_for_completions(const char *path, uint64_t total)
{
    struct rico_info info;
    int fd = open_dev(path, 0);
    for (int i = 0; i < 5000; i++) {
        if (ioctl(fd, RICO_IOC_GETINFO, &info)) { CHECK(0, "GETINFO: %s", strerror(errno)); break; }
        if (info.completed >= total) break;
        usleep(1000);
    }
    CHECK(info.submitted == total, "device submitted %llu, want %llu",
          (unsigned long long)info.submitted, (unsigned long long)total);
    CHECK(info.completed == total, "device completed %llu, want %llu",
          (unsigned long long)info.completed, (unsigned long long)total);
    CHECK(info.bad_cpl == 0, "bad completions: %llu", (unsigned long long)info.bad_cpl);
    close(fd);
}

static void verify(const struct rico_cqe *c, uint8_t *model, unsigned char *seen, uint64_t m)
{
    uint64_t id = c->user_data;
    struct rico_req e;

    if (id >= m || seen[id]) { CHECK(0, "unknown/duplicate completion id %llu", (unsigned long long)id); return; }
    seen[id] = 1;
    rico_make_req(id, &e);
    CHECK(c->status == RICO_ST_OK, "id %llu status %u", (unsigned long long)id, c->status);
    CHECK(c->tsc_submit != 0, "id %llu tsc_submit 0", (unsigned long long)id);
    if (e.op == RICO_OP_READ) {
        CHECK(c->len == e.len && !memcmp(c->data, &model[e.offset], e.len),
              "id %llu READ data mismatch (len %u want %u)", (unsigned long long)id, c->len, e.len);
    } else {
        CHECK(c->len == 0, "id %llu len %u on non-read", (unsigned long long)id, c->len);
    }
    if (e.op == RICO_OP_WRITE)
        memcpy(&model[e.offset], e.payload, e.len);
}

static void test_main(const char *path, uint64_t m, unsigned depth)
{
    uint8_t *model = calloc(1, RICO_DEV_BYTES);
    unsigned char *seen = calloc(1, m);
    struct rico_sqe sqe[BATCH];
    struct rico_cqe cqe[BATCH * 2];
    uint64_t submitted = 0, completed = 0;
    unsigned iter = 0;
    int fd = open_dev(path, 0);
    struct timespec t0, t1;

    if (!model || !seen) { perror("calloc"); exit(1); }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (completed < m) {
        /* submit as many as depth allows, in batches */
        while (submitted < m && submitted - completed < depth) {
            uint64_t room = depth - (submitted - completed);
            unsigned nb = room < BATCH ? (unsigned)room : BATCH;
            if (m - submitted < nb) nb = (unsigned)(m - submitted);
            for (unsigned i = 0; i < nb; i++) sqe_from_req(&sqe[i], submitted + i);
            ssize_t r = write(fd, sqe, nb * sizeof sqe[0]);
            if (r < 0 && errno == EAGAIN) break;    /* slots all held: go reap */
            if (r < 0) { CHECK(0, "write: %s", strerror(errno)); return; }
            CHECK(r % sizeof sqe[0] == 0, "write returned partial record");
            submitted += r / sizeof sqe[0];
            if ((size_t)r < nb * sizeof sqe[0]) break;
        }
        /* alternate the three ways of waiting */
        switch (iter++ % 3) {
        case 0: {
            struct pollfd p = { .fd = fd, .events = POLLIN };
            int pr = poll(&p, 1, 5000);
            CHECK(pr == 1 && (p.revents & POLLIN), "poll: %d revents %x", pr, p.revents);
            if (pr != 1) return;
            break;
        }
        case 1: {
            struct rico_wait w = { .min_complete = 1, .timeout_us = 5000000 };
            if (ioctl(fd, RICO_IOC_WAIT, &w)) { CHECK(0, "WAIT: %s", strerror(errno)); return; }
            CHECK(w.ready >= 1, "WAIT ready %u", w.ready);
            break;
        }
        default:
            break;                           /* straight to a blocking read */
        }
        ssize_t r = read(fd, cqe, sizeof cqe);
        if (r < 0) { CHECK(0, "read: %s", strerror(errno)); return; }
        CHECK(r % sizeof cqe[0] == 0, "read returned partial record");
        for (size_t i = 0; i < (size_t)r / sizeof cqe[0]; i++, completed++)
            verify(&cqe[i], model, seen, m);
        if (errors > 50) { fprintf(stderr, "drvtest: too many errors, aborting\n"); return; }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
    printf("drvtest: main phase %llu requests, depth %u, %.3f s (%.0f req/s)\n",
           (unsigned long long)m, depth, s, m / s);

    /* nothing further may be pending on this fd */
    {
        int nb = open_dev(path, O_NONBLOCK);
        errno = 0;
        CHECK(read(nb, cqe, sizeof cqe) == -1 && errno == EAGAIN, "extra completions after main phase");
        close(nb);
    }
    close(fd);
    free(model); free(seen);
}

int main(int argc, char **argv)
{
    const char *path = "/dev/rico";
    uint64_t n = 0;
    unsigned depth = 64;
    int npos = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--depth") && i + 1 < argc) depth = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (argv[i][0] == '/') path = argv[i];
        else if (npos++ == 0) n = strtoull(argv[i], NULL, 0);
    }
    const uint64_t fixed = BP_COUNT + ORPHANS;
    if (n < fixed + 1000 || depth < 1 || depth > BP_COUNT - ORPHANS) {
        fprintf(stderr, "usage: %s [/dev/rico] <n >= %llu> [--depth 1..%u]\n", argv[0],
                (unsigned long long)(fixed + 1000), BP_COUNT - ORPHANS);
        return 2;
    }
    signal(SIGALRM, on_alarm);
    alarm(120);

    test_errors(path);
    test_backpressure(path);
    test_orphans(path);
    test_main(path, n - fixed, depth);
    wait_for_completions(path, n);

    printf("drvtest: %llu requests, %lu errors\n", (unsigned long long)n, errors);
    puts(errors ? "drvtest: FAIL" : "drvtest: PASS");
    return errors ? 1 : 0;
}

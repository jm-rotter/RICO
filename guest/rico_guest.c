/*
 * rico_guest.c - guest-side stub: map the shared region, push requests, and
 * consume + verify completions.
 *
 *   rico_guest <path> [n] [--depth D] [--pingpong] [--cpu N] [--warmup W]
 *     --pingpong  same as --depth 1: unloaded round-trip latency
 *     --depth D   max requests in flight (default unbounded = loaded)
 *     --cpu N     pin to CPU N
 *
 * Inside a VM, <path> is the ivshmem BAR2 resource file, e.g.
 *   /sys/bus/pci/devices/0000:00:04.0/resource2      (run as root)
 * On the host (no VM) it can be /dev/shm/rico to test the stub standalone.
 *
 * Note on mapping type: resource2 is mapped uncached, which is slow but safe
 * with plain release stores. Do NOT switch to resource2_wc without adding an
 * sfence before publishing `tail`: on x86 a release store is only a compiler
 * barrier, and write-combining stores can drain out of order.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "rico_guest.h"

int main(int argc, char **argv)
{
    const char *path = NULL;
    struct rico_guest_cfg cfg = { .n = 1000000, .depth = 0, .warmup = (uint64_t)-1, .cpu = -1 };
    int npos = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pingpong")) cfg.depth = 1;
        else if (!strcmp(argv[i], "--depth") && i + 1 < argc) cfg.depth = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--cpu") && i + 1 < argc) cfg.cpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) cfg.warmup = strtoull(argv[++i], NULL, 0);
        else if (npos++ == 0) path = argv[i];
        else cfg.n = strtoull(argv[i], NULL, 0);
    }
    if (!path) { fprintf(stderr, "usage: %s <bar2-or-shm-path> [n] [--pingpong|--depth D] [--cpu N] [--warmup W]\n", argv[0]); return 2; }
    if (cfg.warmup == (uint64_t)-1) cfg.warmup = cfg.n / 10 > 10000 ? 10000 : cfg.n / 10;
    cfg.cq_leg = 0;                         /* host TSC is a different domain in a VM */

    int fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0) { perror("open"); return 1; }
    struct rico_shm *s = mmap(NULL, RICO_SHM_SIZE, PROT_READ | PROT_WRITE,
                              MAP_SHARED, fd, 0);
    if (s == MAP_FAILED) { perror("mmap"); return 1; }

    /* Wait for the host to have initialised the region, then sanity-check it. */
    for (int i = 0; !atomic_load_explicit(&s->hdr.host_ready, memory_order_acquire); i++) {
        if (i > 5000) { fprintf(stderr, "host not ready\n"); return 1; }
        usleep(1000);
    }
    if (s->hdr.magic != RICO_SHM_MAGIC || s->hdr.version != RICO_SHM_VER ||
        s->hdr.slots != RICO_RING_SLOTS || s->hdr.req_size != sizeof(struct rico_req) ||
        s->hdr.cq_slots != RICO_CQ_SLOTS || s->hdr.cpl_size != sizeof(struct rico_cpl)) {
        fprintf(stderr, "bad header (magic %08x ver %u)\n", s->hdr.magic, s->hdr.version);
        return 1;
    }

    struct rico_guest_result r;
    uint64_t errs = rico_guest_run(s, &cfg, &r);
    printf("guest: %llu submitted, %llu completed, %llu errors\n",
           (unsigned long long)r.submitted, (unsigned long long)r.completed,
           (unsigned long long)r.errors);
    rico_guest_report(&cfg, &r, rico_tsc_hz());
    return errs ? 1 : 0;
}

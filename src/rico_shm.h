/*
 * rico_shm.h - layout of the shared region exposed to the guest, plus the
 * deterministic mock workload used by both sides of the tests.
 *
 * The region is backed by a host file (e.g. /dev/shm/rico) that QEMU exposes to
 * the guest as an ivshmem-plain PCI BAR2. ivshmem needs a power-of-two size, so
 * the struct is padded to RICO_SHM_SIZE. The host creates and initialises it
 * *before* QEMU starts; the guest only attaches.
 */
#ifndef RICO_SHM_H
#define RICO_SHM_H

#include "rico_ring.h"

#define RICO_SHM_MAGIC 0x52494330u          /* "RIC0" */
#define RICO_SHM_VER   3u
#define RICO_SHM_SIZE  (256u * 1024)        /* power of two, >= sizeof(rico_shm) */

struct rico_shm {
    /* Written once by the host at creation; guest treats as read-only. */
    _Alignas(RICO_CACHELINE) struct {
        uint32_t magic, version, slots, req_size, cq_slots, cpl_size;
        _Atomic uint32_t host_ready;        /* host: set (release) after init */
    } hdr;
    struct rico_ring sq;                    /* guest = producer, host = consumer */
    struct rico_cq   cq;                    /* host = producer, guest = consumer */
};
_Static_assert(sizeof(struct rico_shm) <= RICO_SHM_SIZE, "grow RICO_SHM_SIZE");

/* --- deterministic mock workload (guest generates, host verifies) -------- */

#define RICO_DEV_BYTES (1u << 20)

static inline uint64_t rico_splitmix(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

static inline void rico_make_req(uint64_t id, struct rico_req *q)
{
    uint64_t h = rico_splitmix(id);
    memset(q, 0, sizeof *q);
    q->id = id;
    q->op = (h % 8 == 0) ? RICO_OP_FLUSH : (h & 8) ? RICO_OP_READ : RICO_OP_WRITE;
    q->len = (q->op == RICO_OP_FLUSH) ? 0 : 1 + (h >> 8) % RICO_MAX_PAYLOAD;
    q->offset = (h >> 16) % (RICO_DEV_BYTES - RICO_MAX_PAYLOAD);
    for (uint32_t i = 0; i < q->len && q->op == RICO_OP_WRITE; i++)
        q->payload[i] = (uint8_t)rico_splitmix(id * 64 + i);
}

#endif /* RICO_SHM_H */

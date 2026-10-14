/*
 * rico_ring.h - lock-free SPSC rings for the RICO guest<->host channel.
 *
 * Two rings live in one shared region, symmetric in structure:
 *
 *   SQ (submission): guest = producer, host  = consumer, entries = rico_req
 *   CQ (completion): host  = producer, guest = consumer, entries = rico_cpl
 *
 * Both are instantiated from the same RICO_DEFINE_RING template, so they have
 * identical layout rules and identical ordering. No syscalls, locks or RMW
 * atomics on the data path.
 *
 * Layout per ring (each on its own cache line, so the two sides never
 * false-share): [ head ][ tail ][ slots... ]
 * `head` is written only by the consumer, `tail` only by the producer.
 *
 * Memory ordering (the whole correctness argument), per ring:
 *   producer: write slot contents (plain stores)
 *             -> store-RELEASE tail          (publishes the slot)
 *   consumer: load-ACQUIRE tail              (sees slot contents)
 *             -> read slot (plain loads)
 *             -> store-RELEASE head          (hands the slot back)
 *   producer: load-ACQUIRE head              (so it doesn't overwrite a slot
 *                                             the consumer is still reading)
 * Each side reads its OWN index with relaxed ordering since only it writes it.
 * The two rings are independent: there is no ordering guarantee between an SQ
 * entry and a CQ entry other than the causal one (host can only complete what
 * it has consumed).
 *
 * head/tail are free-running 32-bit counters (wrap naturally); slot index is
 * counter & (N-1), so N must be a power of two. Full = tail-head == N.
 *
 * TRUST: the peer is untrusted in both directions (host doesn't trust the
 * guest; in a CVM the guest doesn't trust the host). Pop copies each slot out
 * exactly once and rejects a producer index that claims more than N entries;
 * callers must validate fields of the copy -- never read slots in place.
 */
#ifndef RICO_RING_H
#define RICO_RING_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define RICO_CACHELINE   64
#define RICO_RING_SLOTS  1024u              /* SQ depth; power of two */
#define RICO_CQ_SLOTS    1024u              /* CQ depth; power of two */
#define RICO_MAX_PAYLOAD 32u                /* inline bytes in a request */
#define RICO_MAX_CPL_DATA 32u               /* inline bytes in a completion */

enum rico_op { RICO_OP_READ = 1, RICO_OP_WRITE = 2, RICO_OP_FLUSH = 3 };
enum rico_status { RICO_ST_OK = 0, RICO_ST_INVAL = 1, RICO_ST_IOERR = 2 };

/* SQ entry: one cache line. WRITE: payload[0..len) is data to write.
 * READ: len = number of bytes requested (<= RICO_MAX_PAYLOAD). */
struct rico_req {
    uint64_t id;                            /* guest-assigned sequence number */
    uint32_t op;                            /* enum rico_op */
    uint32_t len;
    uint64_t offset;                        /* device offset */
    uint64_t tsc_submit;                    /* guest TSC just before the push */
    uint8_t  payload[RICO_MAX_PAYLOAD];
} __attribute__((aligned(8)));
_Static_assert(sizeof(struct rico_req) == RICO_CACHELINE,
               "request should be exactly one cache line");

/* CQ entry: one cache line. Echoes the request id; READ data is inline.
 * tsc_complete is in the host's TSC domain: under KVM it differs from the
 * guest TSC by a constant offset, so only compare it with guest stamps when
 * both sides share a clock (same-machine tests), never across a VM boundary. */
struct rico_cpl {
    uint64_t id;                            /* id of the completed request */
    uint32_t status;                        /* enum rico_status */
    uint32_t len;                           /* valid bytes in data (<= MAX) */
    uint64_t tsc_submit;                    /* echoed from the request, untouched */
    uint64_t tsc_complete;                  /* HOST TSC just before the CQ push */
    uint8_t  data[RICO_MAX_CPL_DATA];
} __attribute__((aligned(8)));
_Static_assert(sizeof(struct rico_cpl) == RICO_CACHELINE,
               "completion should be exactly one cache line");

/*
 * RICO_DEFINE_RING(NAME, ENTRY, SLOTS) defines:
 *   struct NAME;
 *   bool NAME_push(struct NAME *, const ENTRY *)   producer; false if full
 *   bool NAME_pop (struct NAME *, ENTRY *)         consumer; false if empty
 *   bool NAME_corrupt(struct NAME *)               consumer: tail impossible
 */
#define RICO_DEFINE_RING(NAME, ENTRY, SLOTS)                                    \
_Static_assert(((SLOTS) & ((SLOTS) - 1)) == 0, #NAME ": SLOTS must be 2^k");    \
struct NAME {                                                                   \
    /* Consumer-owned line */                                                   \
    _Alignas(RICO_CACHELINE) _Atomic uint32_t head;                             \
    /* Producer-owned line */                                                   \
    _Alignas(RICO_CACHELINE) _Atomic uint32_t tail;                             \
    _Alignas(RICO_CACHELINE) ENTRY slots[SLOTS];                                \
};                                                                              \
static inline bool NAME##_push(struct NAME *r, const ENTRY *e)                  \
{                                                                               \
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);       \
    uint32_t head = atomic_load_explicit(&r->head, memory_order_acquire);       \
    if (tail - head == (SLOTS))                                                 \
        return false;                                                           \
    r->slots[tail & ((SLOTS) - 1)] = *e;                    /* plain stores */  \
    atomic_store_explicit(&r->tail, tail + 1, memory_order_release);            \
    return true;                                                                \
}                                                                               \
static inline bool NAME##_corrupt(struct NAME *r)                               \
{                                                                               \
    uint32_t head = atomic_load_explicit(&r->head, memory_order_relaxed);       \
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);       \
    return tail - head > (SLOTS);                                               \
}                                                                               \
static inline bool NAME##_pop(struct NAME *r, ENTRY *out)                       \
{                                                                               \
    uint32_t head = atomic_load_explicit(&r->head, memory_order_relaxed);       \
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);       \
    if (head == tail || tail - head > (SLOTS))                                  \
        return false;                                                           \
    *out = r->slots[head & ((SLOTS) - 1)];                  /* single copy */   \
    atomic_store_explicit(&r->head, head + 1, memory_order_release);            \
    return true;                                                                \
}

RICO_DEFINE_RING(rico_ring, struct rico_req, RICO_RING_SLOTS)   /* SQ */
RICO_DEFINE_RING(rico_cq,   struct rico_cpl, RICO_CQ_SLOTS)     /* CQ */

/*
 * Timestamp counter. lfence first so rdtsc can't execute early, ahead of
 * preceding loads/stores still in flight (rdtsc alone isn't serialising).
 * Costs ~25 cycles; that floor is included in every measurement.
 */
static inline uint64_t rico_rdtsc(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_lfence();
    return __builtin_ia32_rdtsc();
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ __volatile__("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
#error "no cycle counter for this arch"
#endif
}

/* CPU spin hint, so the polling side is polite to its SMT sibling. */
static inline void rico_cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

#endif /* RICO_RING_H */

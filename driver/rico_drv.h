/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _RICO_DRV_H
#define _RICO_DRV_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/build_bug.h>
#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/rwsem.h>
#include <linux/mutex.h>
#include <linux/kref.h>
#include <linux/wait.h>
#include <linux/miscdevice.h>
#include <linux/stddef.h>
#include "rico_uapi.h"

/* ---- wire format: must mirror src/rico_ring.h / src/rico_shm.h ---- */

#define RICO_PCI_VENDOR		0x1af4
#define RICO_PCI_DEVICE		0x1110
#define RICO_BAR		2

#define RICO_CACHELINE		64
#define RICO_RING_SLOTS		1024u
#define RICO_CQ_SLOTS		1024u
#define RICO_SHM_MAGIC		0x52494330u
#define RICO_SHM_VER		3u
#define RICO_SHM_SIZE		(256u * 1024)

struct rico_wire_req {
	u64 id;
	u32 op;
	u32 len;
	u64 offset;
	u64 tsc_submit;
	u8  payload[RICO_MAX_PAYLOAD];
} __aligned(8);

struct rico_wire_cpl {
	u64 id;
	u32 status;
	u32 len;
	u64 tsc_submit;
	u64 tsc_complete;
	u8  data[RICO_MAX_CPL_DATA];
} __aligned(8);

struct rico_hdr_layout {
	u32 magic, version, slots, req_size, cq_slots, cpl_size;
	u32 host_ready;
} __aligned(RICO_CACHELINE);

struct rico_sq_layout {
	u32 head __aligned(RICO_CACHELINE);	/* consumer (host) */
	u32 tail __aligned(RICO_CACHELINE);	/* producer (guest) */
	struct rico_wire_req slots[RICO_RING_SLOTS] __aligned(RICO_CACHELINE);
} __aligned(RICO_CACHELINE);

struct rico_cq_layout {
	u32 head __aligned(RICO_CACHELINE);	/* consumer (guest) */
	u32 tail __aligned(RICO_CACHELINE);	/* producer (host) */
	struct rico_wire_cpl slots[RICO_CQ_SLOTS] __aligned(RICO_CACHELINE);
} __aligned(RICO_CACHELINE);

struct rico_shm_layout {
	struct rico_hdr_layout hdr;
	struct rico_sq_layout  sq;
	struct rico_cq_layout  cq;
};

#define RICO_OFF_HDR_READY	offsetof(struct rico_shm_layout, hdr.host_ready)
#define RICO_OFF_SQ_HEAD	offsetof(struct rico_shm_layout, sq.head)
#define RICO_OFF_SQ_TAIL	offsetof(struct rico_shm_layout, sq.tail)
#define RICO_OFF_SQ_SLOTS	offsetof(struct rico_shm_layout, sq.slots)
#define RICO_OFF_CQ_HEAD	offsetof(struct rico_shm_layout, cq.head)
#define RICO_OFF_CQ_TAIL	offsetof(struct rico_shm_layout, cq.tail)
#define RICO_OFF_CQ_SLOTS	offsetof(struct rico_shm_layout, cq.slots)

/* Literal copies of the host's RICO_WIRE_OFF_* (src/rico_shm.h), where they
 * are asserted against the real C11 struct. If the layout changes, both sides
 * fail to build until updated. */
static_assert(RICO_OFF_HDR_READY == 24,    "wire layout: hdr.host_ready");
static_assert(RICO_OFF_SQ_HEAD   == 64,    "wire layout: sq.head");
static_assert(RICO_OFF_SQ_TAIL   == 128,   "wire layout: sq.tail");
static_assert(RICO_OFF_SQ_SLOTS  == 192,   "wire layout: sq.slots");
static_assert(RICO_OFF_CQ_HEAD   == 65728, "wire layout: cq.head");
static_assert(RICO_OFF_CQ_TAIL   == 65792, "wire layout: cq.tail");
static_assert(RICO_OFF_CQ_SLOTS  == 65856, "wire layout: cq.slots");
static_assert(sizeof(struct rico_wire_req) == 64, "wire layout: rico_req");
static_assert(sizeof(struct rico_wire_cpl) == 64, "wire layout: rico_cpl");

/* ---- driver state ---- */

/* In-flight + unread completions are capped at the SQ depth, so neither ring
 * can overflow while the host honours the protocol. */
#define RICO_MAX_INFLIGHT	RICO_RING_SLOTS

enum rico_slot_state {
	RICO_SLOT_FREE = 0,
	RICO_SLOT_INFLIGHT,
	RICO_SLOT_DONE,
};

struct pci_dev;
struct task_struct;
struct rico_ctx;

struct rico_slot {
	struct list_head node;		/* free list or ctx->done */
	struct rico_ctx *ctx;		/* NULL once the owner has closed */
	u64 user_data;
	u64 tsc_submit;
	u32 gen;
	u32 state;
	struct rico_cqe cqe;
};

struct rico_ctx {			/* one per open(2) */
	struct rico_dev *dev;
	struct list_head node;		/* dev->ctxs */
	struct list_head done;		/* completed, unread */
	u32 ndone;			/* dev->lock */
	u32 ninflight;			/* dev->lock */
	wait_queue_head_t wq;
	struct mutex io_lock;		/* serialises read(2) on this fd */
};

struct rico_dev {
	struct pci_dev *pdev;
	void __iomem *shm;
	struct kref ref;
	struct miscdevice misc;
	struct rw_semaphore hw_sem;	/* read: touching ->shm; write: teardown */

	/* dev->lock guards: dead, broken, slots, free list, nfree, ctxs,
	 * every ctx's done list/counters, counters below. Lock order:
	 * cq_lock -> lock. sq_lock is never nested with lock. */
	spinlock_t lock;
	bool dead;
	bool broken;
	struct list_head free;
	u32 nfree;
	struct list_head ctxs;
	u64 n_completed;
	u64 n_bad;
	struct rico_slot *slots;

	atomic_t inflight;		/* submitted, not yet completed */
	wait_queue_head_t slot_wq;

	spinlock_t sq_lock;		/* SQ producer */
	u32 sq_tail;			/* shadow of the shared tail */
	u64 n_submitted;

	spinlock_t cq_lock;		/* CQ consumer */
	u32 cq_head;			/* shadow of the shared head */

	struct task_struct *poller;
	wait_queue_head_t poll_wq;
};

#endif /* _RICO_DRV_H */

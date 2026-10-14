// SPDX-License-Identifier: GPL-2.0
/*
 * rico.ko - guest-side driver for the RICO ivshmem dual-ring transport.
 *
 * Binds to ivshmem-plain (1af4:1110), maps BAR2 and exposes /dev/rico.
 * Guest = SQ producer / CQ consumer. There is no doorbell or interrupt:
 * completions are found by polling (waiter busy-poll + "rico-poll" kthread).
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/delay.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/timex.h>
#ifdef CONFIG_X86
#include <asm/msr.h>
#if __has_include(<asm/tsc.h>)
#include <asm/tsc.h>		/* rdtsc_ordered() lives here on newer kernels */
#endif
#endif

#include "rico_drv.h"

#define RICO_BATCH	16	/* records staged on the stack per copy */

static unsigned int busy_poll_us = 50;
module_param(busy_poll_us, uint, 0644);
MODULE_PARM_DESC(busy_poll_us, "Waiter busy-poll window before sleeping (us)");

static unsigned int poll_spin_us = 100;
module_param(poll_spin_us, uint, 0644);
MODULE_PARM_DESC(poll_spin_us, "Poller thread spin window after last completion (us)");

static unsigned int poll_sleep_us = 50;
module_param(poll_sleep_us, uint, 0644);
MODULE_PARM_DESC(poll_sleep_us, "Poller thread sleep granularity once idle (us)");

static struct rico_dev *g_dev;	/* single ivshmem instance supported */

static inline u64 rico_tsc(void)
{
#ifdef CONFIG_X86
	return rdtsc_ordered();
#else
	return get_cycles();
#endif
}

static void rico_dev_free(struct kref *ref)
{
	struct rico_dev *dev = container_of(ref, struct rico_dev, ref);

	kvfree(dev->slots);
	kfree(dev);
}

/* ------------------------------------------------------------------ */
/* shared-memory accessors                                             */
/* ------------------------------------------------------------------ */

static inline void __iomem *rico_sq_slot(struct rico_dev *d, u32 idx)
{
	return d->shm + RICO_OFF_SQ_SLOTS +
	       (size_t)(idx & (RICO_RING_SLOTS - 1)) * sizeof(struct rico_wire_req);
}

static inline void __iomem *rico_cq_slot(struct rico_dev *d, u32 idx)
{
	return d->shm + RICO_OFF_CQ_SLOTS +
	       (size_t)(idx & (RICO_CQ_SLOTS - 1)) * sizeof(struct rico_wire_cpl);
}

/* ------------------------------------------------------------------ */
/* state helpers (dev->lock)                                           */
/* ------------------------------------------------------------------ */

static void rico_wake_all_locked(struct rico_dev *dev)
{
	struct rico_ctx *ctx;

	list_for_each_entry(ctx, &dev->ctxs, node)
		wake_up(&ctx->wq);
	wake_up(&dev->slot_wq);
}

static void rico_mark_broken(struct rico_dev *dev, const char *why)
{
	spin_lock(&dev->lock);
	if (!dev->broken) {
		dev->broken = true;
		pr_err("shared ring corrupted by peer (%s); device disabled\n", why);
		rico_wake_all_locked(dev);
	}
	spin_unlock(&dev->lock);
}

static void rico_free_slot_locked(struct rico_dev *dev, struct rico_slot *slot)
{
	slot->state = RICO_SLOT_FREE;
	slot->ctx = NULL;
	list_add(&slot->node, &dev->free);
	dev->nfree++;
}

/* Returns -ENODEV / -EIO when the device is unusable, else 0. */
static int rico_ctx_snapshot(struct rico_ctx *ctx, u32 *ndone, u32 *ninfl, u32 *nfree)
{
	struct rico_dev *dev = ctx->dev;
	int ret = 0;

	spin_lock(&dev->lock);
	if (dev->dead)
		ret = -ENODEV;
	else if (dev->broken)
		ret = -EIO;
	if (ndone)
		*ndone = ctx->ndone;
	if (ninfl)
		*ninfl = ctx->ninflight;
	if (nfree)
		*nfree = dev->nfree;
	spin_unlock(&dev->lock);
	return ret;
}

static bool rico_ctx_ready(struct rico_ctx *ctx, u32 min)
{
	struct rico_dev *dev = ctx->dev;
	bool ready;

	spin_lock(&dev->lock);
	ready = ctx->ndone >= min || dev->dead || dev->broken;
	spin_unlock(&dev->lock);
	return ready;
}

static bool rico_slot_ready(struct rico_ctx *ctx)
{
	struct rico_dev *dev = ctx->dev;
	bool ready;

	spin_lock(&dev->lock);
	ready = dev->nfree || ctx->ndone || dev->dead || dev->broken;
	spin_unlock(&dev->lock);
	return ready;
}

/* ------------------------------------------------------------------ */
/* CQ reaping                                                          */
/* ------------------------------------------------------------------ */

static void rico_deliver(struct rico_dev *dev, const struct rico_wire_cpl *cpl)
{
	struct rico_slot *slot;
	struct rico_ctx *ctx;
	u32 idx = (u32)cpl->id;
	u32 gen = (u32)(cpl->id >> 32);

	spin_lock(&dev->lock);
	if (idx >= RICO_MAX_INFLIGHT) {
		dev->n_bad++;
		goto out;
	}
	slot = &dev->slots[idx];
	if (slot->state != RICO_SLOT_INFLIGHT || slot->gen != gen) {
		dev->n_bad++;		/* forged, duplicate or stale */
		goto out;
	}

	atomic_dec(&dev->inflight);
	dev->n_completed++;
	ctx = slot->ctx;
	if (!ctx) {			/* owner closed: drop */
		rico_free_slot_locked(dev, slot);
		goto wake_slots;
	}

	memset(&slot->cqe, 0, sizeof(slot->cqe));
	slot->cqe.user_data = slot->user_data;
	slot->cqe.tsc_submit = slot->tsc_submit;	/* never trust the echo */
	slot->cqe.tsc_complete = cpl->tsc_complete;
	if (cpl->len > RICO_MAX_CPL_DATA || cpl->status > RICO_ST_IOERR) {
		dev->n_bad++;
		slot->cqe.status = RICO_ST_IOERR;
	} else {
		slot->cqe.status = cpl->status;
		slot->cqe.len = cpl->len;
		memcpy(slot->cqe.data, cpl->data, cpl->len);
	}

	slot->state = RICO_SLOT_DONE;
	list_add_tail(&slot->node, &ctx->done);
	ctx->ndone++;
	ctx->ninflight--;
	wake_up(&ctx->wq);		/* under lock: ctx cannot be freed */
wake_slots:
	/* Unconditional: a lockless waitqueue_active() check here could miss a
	 * waiter that is between prepare_to_wait() and re-checking its condition. */
	wake_up(&dev->slot_wq);
out:
	spin_unlock(&dev->lock);
}

/* Caller holds hw_sem for read. Returns number of CQEs consumed. */
static unsigned int rico_reap(struct rico_dev *dev)
{
	struct rico_wire_cpl cpl;
	u32 head, tail, avail, n = 0;

	if (!spin_trylock(&dev->cq_lock))
		return 0;		/* someone else is reaping */

	head = dev->cq_head;
	tail = readl(dev->shm + RICO_OFF_CQ_TAIL);	/* acquire (see readl) */
	avail = tail - head;
	if (unlikely(avail > RICO_CQ_SLOTS)) {
		rico_mark_broken(dev, "cq tail");
		goto out;
	}

	while (n < avail) {
		memcpy_fromio(&cpl, rico_cq_slot(dev, head + n), sizeof(cpl));
		rico_deliver(dev, &cpl);
		n++;
	}
	if (n) {
		mb();			/* slot loads complete before releasing them */
		dev->cq_head = head + n;
		writel(head + n, dev->shm + RICO_OFF_CQ_HEAD);
	}
out:
	spin_unlock(&dev->cq_lock);
	return n;
}

static inline void rico_reap_if_pending(struct rico_dev *dev)
{
	if (atomic_read(&dev->inflight))
		rico_reap(dev);
}

static int rico_poller(void *data)
{
	struct rico_dev *dev = data;

	while (!kthread_should_stop()) {
		ktime_t idle_since;

		wait_event_idle(dev->poll_wq,
				kthread_should_stop() || READ_ONCE(dev->dead) ||
				atomic_read(&dev->inflight) > 0);
		if (kthread_should_stop())
			break;

		idle_since = ktime_get();
		while (!kthread_should_stop() && atomic_read(&dev->inflight) > 0) {
			unsigned int n = 0;

			down_read(&dev->hw_sem);
			if (dev->dead || dev->broken) {
				up_read(&dev->hw_sem);
				break;
			}
			n = rico_reap(dev);
			up_read(&dev->hw_sem);

			if (n) {
				idle_since = ktime_get();
			} else if (ktime_us_delta(ktime_get(), idle_since) <
				   (s64)READ_ONCE(poll_spin_us)) {
				cpu_relax();
			} else {
				unsigned int us = max(1u, READ_ONCE(poll_sleep_us));

				usleep_range(us, us * 2);
			}
			cond_resched();
		}
		/* dead/broken with inflight > 0: avoid a hot loop */
		if (READ_ONCE(dev->dead) || READ_ONCE(dev->broken))
			schedule_timeout_idle(HZ / 10);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* submit                                                              */
/* ------------------------------------------------------------------ */

/* Returns 0, -EBUSY (no free slot), -EAGAIN (SQ full), -EIO, -ENODEV, -EINVAL.
 * Caller holds hw_sem for read. */
static int rico_submit_one(struct rico_ctx *ctx, const struct rico_sqe *sqe)
{
	struct rico_dev *dev = ctx->dev;
	struct rico_wire_req req;
	struct rico_slot *slot;
	u32 head, tail, gen, idx;
	int ret;

	if (sqe->reserved)
		return -EINVAL;
	switch (sqe->op) {
	case RICO_OP_READ:
	case RICO_OP_WRITE:
		if (!sqe->len || sqe->len > RICO_MAX_PAYLOAD)
			return -EINVAL;
		break;
	case RICO_OP_FLUSH:
		if (sqe->len)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	spin_lock(&dev->lock);
	if (dev->dead) {
		spin_unlock(&dev->lock);
		return -ENODEV;
	}
	if (dev->broken) {
		spin_unlock(&dev->lock);
		return -EIO;
	}
	slot = list_first_entry_or_null(&dev->free, struct rico_slot, node);
	if (!slot) {
		spin_unlock(&dev->lock);
		return -EBUSY;
	}
	list_del(&slot->node);
	dev->nfree--;
	gen = ++slot->gen;
	idx = slot - dev->slots;
	slot->ctx = ctx;
	slot->user_data = sqe->user_data;
	slot->state = RICO_SLOT_INFLIGHT;
	ctx->ninflight++;
	atomic_inc(&dev->inflight);
	spin_unlock(&dev->lock);

	memset(&req, 0, sizeof(req));
	req.id = ((u64)gen << 32) | idx;
	req.op = sqe->op;
	req.len = sqe->len;
	req.offset = sqe->offset;
	if (sqe->op == RICO_OP_WRITE)
		memcpy(req.payload, sqe->payload, sqe->len);

	spin_lock(&dev->sq_lock);
	head = readl(dev->shm + RICO_OFF_SQ_HEAD);
	tail = dev->sq_tail;
	if (unlikely(tail - head >= RICO_RING_SLOTS)) {
		ret = (tail - head > RICO_RING_SLOTS) ? -EIO : -EAGAIN;
		spin_unlock(&dev->sq_lock);
		goto undo;
	}
	req.tsc_submit = rico_tsc();
	slot->tsc_submit = req.tsc_submit;	/* before publish; reaper reads after */
	memcpy_toio(rico_sq_slot(dev, tail), &req, sizeof(req));
	wmb();					/* slot visible before tail */
	dev->sq_tail = tail + 1;
	writel(tail + 1, dev->shm + RICO_OFF_SQ_TAIL);
	dev->n_submitted++;
	spin_unlock(&dev->sq_lock);
	return 0;

undo:
	if (ret == -EIO)
		rico_mark_broken(dev, "sq head");
	spin_lock(&dev->lock);
	atomic_dec(&dev->inflight);
	ctx->ninflight--;
	rico_free_slot_locked(dev, slot);
	spin_unlock(&dev->lock);
	return ret;
}

/* ------------------------------------------------------------------ */
/* waiting                                                             */
/* ------------------------------------------------------------------ */

/* Wait until ctx has >= min readable completions. Caller holds hw_sem (read). */
static int rico_wait_complete(struct rico_ctx *ctx, u32 min, u64 timeout_us,
			      bool nonblock)
{
	struct rico_dev *dev = ctx->dev;
	ktime_t start = ktime_get();
	ktime_t spin_end = ktime_add_us(start, READ_ONCE(busy_poll_us));
	ktime_t deadline = timeout_us ? ktime_add_us(start, timeout_us) : 0;
	u32 ndone, ninfl;
	int ret;

	for (;;) {
		ktime_t now;

		rico_reap_if_pending(dev);
		ret = rico_ctx_snapshot(ctx, &ndone, &ninfl, NULL);
		if (ret)
			return ret;
		if (ndone >= min)
			return 0;
		if (nonblock)
			return -EAGAIN;
		if (ndone + ninfl < min)
			return -ENODATA;	/* nothing outstanding can satisfy it */
		if (signal_pending(current))
			return -ERESTARTSYS;

		now = ktime_get();
		if (deadline && ktime_compare(now, deadline) >= 0)
			return -ETIME;

		if (ktime_compare(now, spin_end) < 0 && !need_resched()) {
			cpu_relax();
			continue;
		}

		/* The poller thread wakes us; the timeout is only a safety net. */
		ret = wait_event_interruptible_timeout(ctx->wq,
						       rico_ctx_ready(ctx, min),
						       msecs_to_jiffies(10));
		if (ret < 0)
			return -ERESTARTSYS;
	}
}

static int rico_wait_slot(struct rico_ctx *ctx, bool nonblock)
{
	u32 ndone, nfree;
	int ret;

	for (;;) {
		ret = rico_ctx_snapshot(ctx, &ndone, NULL, &nfree);
		if (ret)
			return ret;
		if (nfree)
			return 0;
		/* All capacity is held by unread completions (maybe ours):
		 * blocking would deadlock a single-threaded caller. */
		if (nonblock || ndone)
			return -EAGAIN;
		if (wait_event_interruptible(ctx->dev->slot_wq, rico_slot_ready(ctx)))
			return -ERESTARTSYS;
	}
}

/* ------------------------------------------------------------------ */
/* file operations                                                     */
/* ------------------------------------------------------------------ */

static int rico_open(struct inode *inode, struct file *file)
{
	struct rico_dev *dev = container_of(file->private_data, struct rico_dev, misc);
	struct rico_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->dev = dev;
	INIT_LIST_HEAD(&ctx->done);
	init_waitqueue_head(&ctx->wq);
	mutex_init(&ctx->io_lock);

	spin_lock(&dev->lock);
	if (dev->dead) {
		spin_unlock(&dev->lock);
		kfree(ctx);
		return -ENODEV;
	}
	list_add(&ctx->node, &dev->ctxs);
	spin_unlock(&dev->lock);

	kref_get(&dev->ref);
	file->private_data = ctx;
	return nonseekable_open(inode, file);
}

static int rico_release(struct inode *inode, struct file *file)
{
	struct rico_ctx *ctx = file->private_data;
	struct rico_dev *dev = ctx->dev;
	struct rico_slot *slot, *tmp;
	u32 i;

	spin_lock(&dev->lock);
	list_del(&ctx->node);
	list_for_each_entry_safe(slot, tmp, &ctx->done, node) {
		list_del(&slot->node);
		rico_free_slot_locked(dev, slot);
	}
	/* Still in flight: orphan; the reaper frees them on completion. */
	for (i = 0; i < RICO_MAX_INFLIGHT; i++)
		if (dev->slots[i].ctx == ctx)
			dev->slots[i].ctx = NULL;
	spin_unlock(&dev->lock);
	wake_up(&dev->slot_wq);

	kfree(ctx);
	kref_put(&dev->ref, rico_dev_free);
	return 0;
}

static ssize_t rico_write(struct file *file, const char __user *ubuf,
			  size_t count, loff_t *ppos)
{
	struct rico_ctx *ctx = file->private_data;
	struct rico_dev *dev = ctx->dev;
	bool nonblock = file->f_flags & O_NONBLOCK;
	struct rico_sqe sqe[RICO_BATCH];
	size_t done = 0;
	int ret = 0;

	if (count % sizeof(struct rico_sqe))
		return -EINVAL;
	if (!count)
		return 0;

	down_read(&dev->hw_sem);
	ret = rico_ctx_snapshot(ctx, NULL, NULL, NULL);
	if (ret)
		goto out;

	while (done < count) {
		size_t chunk = min_t(size_t, count - done, sizeof(sqe));
		size_t i;

		if (copy_from_user(sqe, ubuf + done, chunk)) {
			ret = -EFAULT;
			goto out;
		}
		for (i = 0; i < chunk / sizeof(sqe[0]); i++) {
			while ((ret = rico_submit_one(ctx, &sqe[i])) == -EBUSY) {
				if (done)		/* report partial progress */
					goto out_ok;
				ret = rico_wait_slot(ctx, nonblock);
				if (ret)
					goto out;
			}
			if (ret)
				goto out;
			done += sizeof(sqe[0]);
		}
	}
out_ok:
	ret = 0;
out:
	if (done)
		wake_up(&dev->poll_wq);
	up_read(&dev->hw_sem);
	return done ? (ssize_t)done : ret;
}

static ssize_t rico_read(struct file *file, char __user *ubuf,
			 size_t count, loff_t *ppos)
{
	struct rico_ctx *ctx = file->private_data;
	struct rico_dev *dev = ctx->dev;
	bool nonblock = file->f_flags & O_NONBLOCK;
	struct rico_cqe batch[RICO_BATCH];
	size_t max = count / sizeof(struct rico_cqe), done = 0;
	int ret;

	if (!max)
		return -EINVAL;

	down_read(&dev->hw_sem);
	mutex_lock(&ctx->io_lock);
	ret = rico_wait_complete(ctx, 1, 0, nonblock);
	if (ret)
		goto out;

	while (done < max) {
		struct rico_slot *slot;
		size_t n = 0, want = min_t(size_t, max - done, RICO_BATCH);

		/* Peek without consuming: a faulting copy_to_user loses nothing.
		 * io_lock guarantees no one else removes from ctx->done. */
		spin_lock(&dev->lock);
		list_for_each_entry(slot, &ctx->done, node) {
			if (n == want)
				break;
			batch[n++] = slot->cqe;
		}
		spin_unlock(&dev->lock);
		if (!n)
			break;

		if (copy_to_user(ubuf + done * sizeof(batch[0]), batch,
				 n * sizeof(batch[0]))) {
			ret = -EFAULT;
			goto out;
		}

		spin_lock(&dev->lock);
		while (n--) {
			slot = list_first_entry(&ctx->done, struct rico_slot, node);
			list_del(&slot->node);
			ctx->ndone--;
			rico_free_slot_locked(dev, slot);
			done++;
		}
		spin_unlock(&dev->lock);
		wake_up(&dev->slot_wq);
	}
	ret = 0;
out:
	mutex_unlock(&ctx->io_lock);
	up_read(&dev->hw_sem);
	return done ? (ssize_t)(done * sizeof(struct rico_cqe)) : ret;
}

static __poll_t rico_poll(struct file *file, poll_table *wait)
{
	struct rico_ctx *ctx = file->private_data;
	struct rico_dev *dev = ctx->dev;
	__poll_t mask = 0;
	u32 ndone, nfree;

	poll_wait(file, &ctx->wq, wait);
	poll_wait(file, &dev->slot_wq, wait);

	if (down_read_trylock(&dev->hw_sem)) {
		if (!READ_ONCE(dev->dead) && !READ_ONCE(dev->broken))
			rico_reap_if_pending(dev);
		up_read(&dev->hw_sem);
	}
	if (rico_ctx_snapshot(ctx, &ndone, NULL, &nfree))
		return EPOLLERR | EPOLLHUP;
	if (ndone)
		mask |= EPOLLIN | EPOLLRDNORM;
	if (nfree)
		mask |= EPOLLOUT | EPOLLWRNORM;
	return mask;
}

static long rico_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct rico_ctx *ctx = file->private_data;
	struct rico_dev *dev = ctx->dev;
	void __user *uarg = (void __user *)arg;
	long ret;

	switch (cmd) {
	case RICO_IOC_GETINFO: {
		struct rico_info info = {
			.abi_version = RICO_ABI_VERSION,
			.sq_slots = RICO_RING_SLOTS,
			.cq_slots = RICO_CQ_SLOTS,
			.max_inflight = RICO_MAX_INFLIGHT,
			.shm_size = RICO_SHM_SIZE,
		};

		spin_lock(&dev->lock);
		info.completed = dev->n_completed;
		info.bad_cpl = dev->n_bad;
		spin_unlock(&dev->lock);
		spin_lock(&dev->sq_lock);
		info.submitted = dev->n_submitted;
		spin_unlock(&dev->sq_lock);
		return copy_to_user(uarg, &info, sizeof(info)) ? -EFAULT : 0;
	}
	case RICO_IOC_WAIT: {
		struct rico_wait w;
		u32 ndone;

		if (copy_from_user(&w, uarg, sizeof(w)))
			return -EFAULT;
		if (w.flags || !w.min_complete)
			return -EINVAL;

		down_read(&dev->hw_sem);
		ret = rico_ctx_snapshot(ctx, NULL, NULL, NULL);
		if (!ret)
			ret = rico_wait_complete(ctx, w.min_complete, w.timeout_us,
						 file->f_flags & O_NONBLOCK);
		up_read(&dev->hw_sem);

		if (rico_ctx_snapshot(ctx, &ndone, NULL, NULL) == 0)
			w.ready = ndone;
		if (copy_to_user(uarg, &w, sizeof(w)))
			return -EFAULT;
		return ret;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations rico_fops = {
	.owner		= THIS_MODULE,
	.open		= rico_open,
	.release	= rico_release,
	.read		= rico_read,
	.write		= rico_write,
	.poll		= rico_poll,
	.unlocked_ioctl	= rico_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	/* no .llseek: nonseekable_open() in rico_open() makes lseek fail ESPIPE */
};

/* ------------------------------------------------------------------ */
/* PCI glue                                                            */
/* ------------------------------------------------------------------ */

static int rico_check_header(struct rico_dev *dev)
{
	struct rico_hdr_layout hdr;
	int i;

	for (i = 0; i < 100; i++) {
		if (readl(dev->shm + RICO_OFF_HDR_READY))
			break;
		msleep(10);
	}
	if (i == 100) {
		pr_err("host not ready (host_ready == 0)\n");
		return -ENODEV;
	}
	rmb();	/* host_ready (acquire) before reading the rest of the header */
	memcpy_fromio(&hdr, dev->shm, sizeof(hdr));

	if (hdr.magic != RICO_SHM_MAGIC || hdr.version != RICO_SHM_VER ||
	    hdr.slots != RICO_RING_SLOTS || hdr.cq_slots != RICO_CQ_SLOTS ||
	    hdr.req_size != sizeof(struct rico_wire_req) ||
	    hdr.cpl_size != sizeof(struct rico_wire_cpl)) {
		pr_err("header mismatch: magic %#x ver %u slots %u/%u sizes %u/%u\n",
		       hdr.magic, hdr.version, hdr.slots, hdr.cq_slots,
		       hdr.req_size, hdr.cpl_size);
		return -EPROTO;
	}
	return 0;
}

static int rico_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct rico_dev *dev;
	u32 sq_head, cq_tail, i;
	int ret;

	BUILD_BUG_ON(sizeof(struct rico_wire_req) != 64);
	BUILD_BUG_ON(sizeof(struct rico_wire_cpl) != 64);
	BUILD_BUG_ON(sizeof(struct rico_sqe) != 64);
	BUILD_BUG_ON(sizeof(struct rico_cqe) != 64);
	BUILD_BUG_ON(sizeof(struct rico_shm_layout) > RICO_SHM_SIZE);

	if (g_dev)
		return -EBUSY;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	dev->slots = kvcalloc(RICO_MAX_INFLIGHT, sizeof(*dev->slots), GFP_KERNEL);
	if (!dev->slots) {
		kfree(dev);
		return -ENOMEM;
	}
	dev->pdev = pdev;
	kref_init(&dev->ref);
	init_rwsem(&dev->hw_sem);
	spin_lock_init(&dev->lock);
	spin_lock_init(&dev->sq_lock);
	spin_lock_init(&dev->cq_lock);
	INIT_LIST_HEAD(&dev->free);
	INIT_LIST_HEAD(&dev->ctxs);
	init_waitqueue_head(&dev->slot_wq);
	init_waitqueue_head(&dev->poll_wq);
	atomic_set(&dev->inflight, 0);

	/* Random starting generations make completions left over from a
	 * previous module load very unlikely to match a live slot. */
	for (i = 0; i < RICO_MAX_INFLIGHT; i++) {
		dev->slots[i].gen = get_random_u32();
		list_add_tail(&dev->slots[i].node, &dev->free);
	}
	dev->nfree = RICO_MAX_INFLIGHT;

	ret = pci_enable_device(pdev);
	if (ret)
		goto err_free;
	ret = pci_request_region(pdev, RICO_BAR, KBUILD_MODNAME);
	if (ret)
		goto err_disable;
	if (pci_resource_len(pdev, RICO_BAR) < RICO_SHM_SIZE) {
		pr_err("BAR%d is %llu bytes, need %u\n", RICO_BAR,
		       (u64)pci_resource_len(pdev, RICO_BAR), RICO_SHM_SIZE);
		ret = -ENODEV;
		goto err_region;
	}
	/* UC mapping, as with sysfs resource2. For pci_iomap_wc() the wmb()/mb()
	 * calls on the data path already provide the needed sfence. */
	dev->shm = pci_iomap_range(pdev, RICO_BAR, 0, RICO_SHM_SIZE);
	if (!dev->shm) {
		ret = -ENOMEM;
		goto err_region;
	}

	ret = rico_check_header(dev);
	if (ret)
		goto err_unmap;

	/* Resume from whatever indices the shared region currently holds. */
	dev->sq_tail = readl(dev->shm + RICO_OFF_SQ_TAIL);
	sq_head = readl(dev->shm + RICO_OFF_SQ_HEAD);
	dev->cq_head = readl(dev->shm + RICO_OFF_CQ_HEAD);
	cq_tail = readl(dev->shm + RICO_OFF_CQ_TAIL);
	if (dev->sq_tail - sq_head > RICO_RING_SLOTS ||
	    cq_tail - dev->cq_head > RICO_CQ_SLOTS) {
		pr_err("ring indices corrupt at probe\n");
		ret = -EIO;
		goto err_unmap;
	}

	dev->misc.minor = MISC_DYNAMIC_MINOR;
	dev->misc.name = "rico";
	dev->misc.fops = &rico_fops;
	dev->misc.mode = 0660;

	pci_set_drvdata(pdev, dev);

	dev->poller = kthread_run(rico_poller, dev, "rico-poll");
	if (IS_ERR(dev->poller)) {
		ret = PTR_ERR(dev->poller);
		goto err_unmap;
	}
	ret = misc_register(&dev->misc);
	if (ret)
		goto err_thread;

	g_dev = dev;
	pr_info("bound %s, %u KiB shm, /dev/rico ready\n", pci_name(pdev),
		RICO_SHM_SIZE / 1024);
	return 0;

err_thread:
	kthread_stop(dev->poller);
err_unmap:
	pci_iounmap(pdev, dev->shm);
err_region:
	pci_release_region(pdev, RICO_BAR);
err_disable:
	pci_disable_device(pdev);
err_free:
	kref_put(&dev->ref, rico_dev_free);
	return ret;
}

static void rico_remove(struct pci_dev *pdev)
{
	struct rico_dev *dev = pci_get_drvdata(pdev);

	misc_deregister(&dev->misc);	/* no new opens after this */
	g_dev = NULL;

	spin_lock(&dev->lock);
	dev->dead = true;
	rico_wake_all_locked(dev);
	spin_unlock(&dev->lock);

	kthread_stop(dev->poller);

	/* Waits for in-flight file operations; they bail out on ->dead. */
	down_write(&dev->hw_sem);
	pci_iounmap(pdev, dev->shm);
	dev->shm = NULL;
	up_write(&dev->hw_sem);

	pci_release_region(pdev, RICO_BAR);
	pci_disable_device(pdev);
	pr_info("unbound %s (submitted %llu, completed %llu, bad completions %llu)\n",
		pci_name(pdev), dev->n_submitted, dev->n_completed, dev->n_bad);
	kref_put(&dev->ref, rico_dev_free);	/* open files hold their own refs */
}

static const struct pci_device_id rico_ids[] = {
	{ PCI_DEVICE(RICO_PCI_VENDOR, RICO_PCI_DEVICE) },
	{ }
};
MODULE_DEVICE_TABLE(pci, rico_ids);

static struct pci_driver rico_pci_driver = {
	.name		= KBUILD_MODNAME,
	.id_table	= rico_ids,
	.probe		= rico_probe,
	.remove		= rico_remove,
};
module_pci_driver(rico_pci_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Project RICO");
MODULE_DESCRIPTION("RICO ivshmem SPSC dual-ring guest transport");

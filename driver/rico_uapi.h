/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_RICO_H
#define _UAPI_RICO_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define RICO_ABI_VERSION	1

#define RICO_OP_READ		1
#define RICO_OP_WRITE		2
#define RICO_OP_FLUSH		3

#define RICO_ST_OK		0
#define RICO_ST_INVAL		1
#define RICO_ST_IOERR		2

/* Guarded so userspace can also include src/rico_ring.h (which defines these
 * as 32u) in either order. */
#ifndef RICO_MAX_PAYLOAD
#define RICO_MAX_PAYLOAD	32
#endif
#ifndef RICO_MAX_CPL_DATA
#define RICO_MAX_CPL_DATA	32
#endif

/* write(2) takes an array of these. */
struct rico_sqe {
	__u64 user_data;		/* opaque cookie, returned in the cqe */
	__u32 op;			/* RICO_OP_* */
	__u32 len;			/* READ/WRITE: 1..32, FLUSH: 0 */
	__u64 offset;			/* device offset */
	__u8  payload[RICO_MAX_PAYLOAD]; /* WRITE data */
	__u64 reserved;			/* must be 0 */
};

/* read(2) returns an array of these. */
struct rico_cqe {
	__u64 user_data;
	__u32 status;			/* RICO_ST_* */
	__u32 len;			/* valid bytes in data */
	__u64 tsc_submit;		/* guest cycle counter at submit */
	__u64 tsc_complete;		/* HOST cycle counter (different domain) */
	__u8  data[RICO_MAX_CPL_DATA];
};

struct rico_info {
	__u32 abi_version;
	__u32 sq_slots;
	__u32 cq_slots;
	__u32 max_inflight;
	__u64 shm_size;
	__u64 submitted;
	__u64 completed;
	__u64 bad_cpl;
};

struct rico_wait {
	__u32 min_complete;		/* wait until this many cqes are readable */
	__u32 flags;			/* must be 0 */
	__u64 timeout_us;		/* 0 = no timeout */
	__u32 ready;			/* out: cqes readable now */
	__u32 pad;
};

#define RICO_IOC_MAGIC		'R'
#define RICO_IOC_GETINFO	_IOR(RICO_IOC_MAGIC, 0x01, struct rico_info)
#define RICO_IOC_WAIT		_IOWR(RICO_IOC_MAGIC, 0x02, struct rico_wait)

#endif /* _UAPI_RICO_H */

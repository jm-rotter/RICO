# Architecture

## Components

| Component | File | Role |
|---|---|---|
| Rings | `src/rico_ring.h` | `RICO_DEFINE_RING` template, instantiated as SQ (`rico_ring`, entries `rico_req`) and CQ (`rico_cq`, entries `rico_cpl`) |
| Region layout | `src/rico_shm.h` | header (magic, version, geometry, `host_ready`) + SQ + CQ, padded to 256 KiB; also the deterministic mock workload |
| Host worker | `host/rico_host.{h,c}` | maps the file, polls SQ, validates, calls a handler, pushes CQ; optional CPU pin, busy-poll |
| Host daemon | `host/rico_hostd.c` | creates + initialises the file, runs the worker over `rico_mockdev.h` or, with `--backing`, the io_uring backend |
| io_uring backend | `host/rico_iouring.{h,c}`, `host/rico_backend.h` | maps READ/WRITE/FLUSH onto a file or block device; reaps the kernel CQ and hands completions back to the worker |
| Guest loop | `guest/rico_guest.h` | submit/complete loop with depth control, in-order verification, TSC stamping |
| Guest stub | `guest/rico_guest.c` | maps BAR2 (or any file), validates header, runs the loop |
| Guest kernel driver | `driver/rico_drv.c`, `rico_uapi.h` | PCI driver for the ivshmem device; `/dev/rico` (write = submit, read/poll/ioctl = reap); polling kthread |
| Driver smoke test | `guest/rico_drvtest.c` | errno/backpressure/orphan/verified-workload test of `/dev/rico` |
| Test VM init | `guest/rico_init.c` | PID 1 of the integration initramfs; optionally insmods `rico.ko`, runs a program, rmmods |

## Shared region

```
offset 0       header   magic "RIC0", version, slot counts, entry sizes, host_ready
               SQ       head (line) | tail (line) | 1024 × rico_req  (64 B)
               CQ       head (line) | tail (line) | 1024 × rico_cpl  (64 B)
size 256 KiB   (ivshmem requires a power of two)
```

Wire offsets (asserted at compile time on both sides: `src/rico_shm.h` against the
C11 struct, `driver/rico_drv.h` against the kernel mirror):

```
hdr.host_ready 24 | sq.head 64  sq.tail 128   sq.slots 192
                  | cq.head 65728  cq.tail 65792  cq.slots 65856
```

The host creates and initialises the backing file **before** QEMU starts
(`memory-backend-file` needs it to exist at the right size). The guest only
attaches. Ring geometry is compile-time on both sides; the header is checked once
at attach and never consulted for sizing afterwards.

## Entries

```
rico_req (64 B): id | op | len | offset | tsc_submit | payload[32]
rico_cpl (64 B): id | status | len | tsc_submit (echo) | tsc_complete (host TSC) | data[32]
```

`op` ∈ {READ, WRITE, FLUSH}. WRITE carries `payload[0..len)`; READ requests
`len` bytes, returned in `data`. A completion is always posted, including
`RICO_ST_INVAL` for rejected requests, so the guest never waits on a dropped one.

## Memory ordering

Per ring, `head` is written only by the consumer and `tail` only by the producer.

| Step | Operation | Ordering |
|---|---|---|
| producer | read own `tail` | relaxed |
| producer | read `head` (is it full?) | acquire — don't overwrite a slot still being read |
| producer | write slot (plain stores) | |
| producer | write `tail` | **release** — publishes the slot |
| consumer | read own `head` | relaxed |
| consumer | read `tail` | **acquire** — sees the slot contents |
| consumer | copy slot (plain loads) | |
| consumer | write `head` | release — returns the slot |

Indices are free-running `uint32_t`; slot = `idx & (N-1)`; full ⇔
`tail - head == N`. The SQ and CQ are independent; the only cross-ring ordering
is causal (the host completes only what it consumed). No RMW atomics, locks or
syscalls are used on the data path.

## Flow control and liveness

- The guest never spins on a full SQ without draining the CQ. If it did, a full
  CQ would stall the host, which would stop draining the SQ: deadlock.
- The host waits (with backoff) on a full CQ and stays responsive to `stop`.
- Idle policy: spin ~4096 polls, then 50 µs sleeps, unless `busy_poll`.
- Depth control (`--depth D`; `--pingpong` = 1) bounds in-flight requests.

## Timestamps

`tsc_submit` is stamped by the guest immediately before the push attempt that
succeeds; the host echoes it untouched. `tsc_complete` is the **host's** TSC just
before the CQ push. Round-trip = guest pop TSC − `tsc_submit` (one clock, valid
across a VM). The CQ-leg (guest pop − host `tsc_complete`) mixes two TSC domains
and is only meaningful when they coincide (same-machine tests); under KVM they
differ by a constant offset, so the guest binary does not report it.
`rico_rdtsc()` is `lfence; rdtsc` (~25 cycles included in every sample).
Timestamps are telemetry only and never used for decisions.

## Extending to QEMU

`rico_host` touches no QEMU internals. A device would call `rico_host_open()` in
`realize()`, run `rico_host_run()` on a `qemu_thread_create()` thread (no BQL), and
call `rico_host_stop()` in `unrealize()`. A custom PCI device (rather than
`ivshmem-plain`) would allow, e.g., exposing the header via BAR0.

## Guest kernel driver

`rico.ko` is the single owner of the guest end of both rings.

- **Mapping:** BAR2 via `pci_iomap_range` (UC); `wmb()` before publishing `tail`
  and `mb()` before releasing `head`, so a write-combining mapping would also be safe.
- **Shadow indices:** the driver keeps its own `sq_tail` / `cq_head`, and treats the
  peer's index as untrusted (more than N entries outstanding = corrupt = device disabled).
- **Request table:** 1024 slots; request id = `generation << 32 | slot`. A completion
  must match an in-flight slot and generation, otherwise it is dropped and counted
  (`bad_cpl`). Slots stay allocated until the completion is read, which bounds
  in-flight + unread to the SQ depth and so prevents ring overflow.
- **Waiting:** no interrupts exist, so a waiter busy-polls `busy_poll_us` (module
  parameter), then sleeps; a `rico-poll` kthread reaps the CQ while anything is in
  flight (spin `poll_spin_us`, then sleep `poll_sleep_us`) and wakes sleepers.
- **Locking:** `sq_lock` (SQ producer), `cq_lock` (CQ consumer, trylock), `lock`
  (table, per-fd done lists), `hw_sem` (read while touching the mapping, write on
  PCI removal). Order: `cq_lock` → `lock`.
- **Teardown:** closing an fd with requests in flight orphans their slots; PCI
  removal marks the device dead, wakes everyone and unmaps under `hw_sem`.

## io_uring backend

`rico_hostd --backing PATH` replaces the mock handler with `host/rico_iouring.c`.
The worker still pops and validates RICO requests; the backend submits them to an
`io_uring` (batched, optional SQPOLL), tracks up to `--qd` requests in flight, holds
back requests that overlap an earlier write (and FLUSH until everything earlier is
done), and returns completions that the worker posts on the RICO CQ. Teardown
cancels and drains in-flight I/O before freeing buffers.

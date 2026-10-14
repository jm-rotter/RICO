# Project RICO

**High-Performance, Exitless Asynchronous SPSC I/O Channels for Confidential VMs**

RICO is a research prototype of a guest↔host I/O channel that moves requests and
completions through shared memory with **no doorbell, no interrupt and no
hypercall on the data path**. Two lock-free single-producer/single-consumer rings
carry commands down and results back; both sides poll.

> **Status: prototype.** It runs and is verified in an ordinary KVM guest, both
> through a raw BAR2 mapping and through a guest kernel driver (`/dev/rico`), with
> the host serving I/O from a mock device or a real file via `io_uring`. It has
> **not** been run inside an actual SEV-SNP/TDX confidential VM, and "no VM exits
> on the data path" is a design property that has not yet been confirmed with
> `kvm_exit` tracing (see [Limitations](#limitations)).

## Overview

Conventional paravirtual I/O (virtio) needs a VM exit to notify the host
("kick") and an interrupt to notify the guest. In a confidential VM each of those
transitions is expensive: the hypervisor can't touch guest-private state, so
exits go through a trusted-context switch (e.g. `#VMEXIT`/`#VC` handling on
SEV-SNP, TD exits on TDX).

RICO removes the notifications instead of speeding them up:

- The guest and host share one 256 KiB region, exposed to the guest as the BAR2
  of a QEMU **`ivshmem-plain`** PCI device, backed by a host file
  (`memory-backend-file,share=on`). It is plain shared RAM: after the first
  touch, loads and stores don't involve the hypervisor.
- A **submission ring (SQ)** carries requests guest → host and a **completion ring
  (CQ)** carries results host → guest. Each is a power-of-two SPSC ring with
  acquire/release ordering and cache-line-separated head/tail.
- A host worker thread polls the SQ; the guest polls the CQ.
- Every entry is exactly one 64-byte cache line.
- Neither side trusts the other: each pops by copying a slot once and validating
  the copy ([threat model](docs/threat-model.md)).
- A guest **kernel module** (`rico.ko`) owns the ring and exposes it as
  `/dev/rico`: `write(2)` submits, `read(2)`/`poll(2)`/`ioctl` reap.
- The host worker can serve requests from a mock device, or from a real file or
  block device through **`io_uring`** (`rico_hostd --backing`).

## Architecture

```
        GUEST (CVM)                                   HOST
 ┌─────────────────────────┐                ┌───────────────────────────┐
 │ application             │                │ rico_hostd (or QEMU thread)│
 │  write/read/poll /dev/rico                │   rico_host_run()          │
 │ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ │                │     poll → validate →      │
 │ rico.ko  (or rico_guest │                │     backend → complete     │
 │  _run() on a raw mmap)  │                │                            │
 │  1. stamp tsc_submit    │                │  pop (copy once, validate) │
 │  2. push ───────────────┼───► SQ ───────►│        │                   │
 │                         │  (64 B slots)  │        ▼                   │
 │  4. pop (copy, verify)◄─┼──── CQ ◄───────┼─ mock device, or io_uring  │
 │  5. wake waiter / RTT   │  (64 B slots)  │  on a file / block device  │
 └───────────┬─────────────┘                └─────────────┬─────────────┘
             │ ioremap / mmap                             │ mmap
   ┌─────────▼─────────────┐                    ┌─────────▼─────────┐
   │ PCI BAR2 (ivshmem)    │◄── same pages ────►│ /dev/shm/rico     │
   └───────────────────────┘   shared memory    └───────────────────┘

   Shared region (256 KiB):  [ header | SQ: head tail slots[1024] | CQ: head tail slots[1024] ]
   Every index and slot array sits on its own cache line(s).
```

Per-ring protocol (full argument in [docs/architecture.md](docs/architecture.md)):

```
producer: write slot  ->  store-release tail      consumer: load-acquire tail -> copy slot -> store-release head
producer: load-acquire head (full check)
```

## Repository layout

| Path | Contents |
|---|---|
| `src/` | Shared headers: `rico_ring.h` (rings), `rico_shm.h` (region layout), `rico_lat.h` (latency stats), `rico_pin.h` (CPU pinning) |
| `host/` | `rico_host.{h,c}` worker library, `rico_hostd.c` daemon, `rico_mockdev.h` mock block device, `rico_iouring.{h,c}` + `rico_backend.h` io_uring storage backend |
| `driver/` | `rico.ko` guest kernel module: `rico_drv.c`, `rico_drv.h` (wire-layout mirror + state), `rico_uapi.h` (userspace ABI), `Makefile` (kbuild) |
| `guest/` | `rico_guest.h` submit/complete loop, `rico_guest.c` raw-BAR stub, `rico_drvtest.c` smoke test for `/dev/rico`, `rico_init.c` PID 1 for the test VM (can also insmod the module), `rico_test.c` stress/benchmark harness |
| `scripts/` | `run_qemu.sh`, `run_local.sh`, `run_integration_test.sh` (raw BAR), `run_driver_test.sh` (kernel module) |
| `docs/` | [Architecture](docs/architecture.md), [threat model](docs/threat-model.md), [evaluation](docs/evaluation/README.md) with raw logs |

## Quickstart

Requirements: Linux x86-64, a C11 compiler, `make`; for the VM tests also
`qemu-system-x86_64` (KVM recommended), `cpio`, `gzip` and a readable
`/boot/vmlinuz-$(uname -r)`.

```sh
make                      # builds into build/
make test                 # stress + ping-pong + ThreadSanitizer, no VM needed
make integration          # full end-to-end test across a real QEMU/KVM boundary
make driver               # build driver/rico.ko (needs kernel headers; KDIR=...)
make driver-test          # boot a VM, insmod rico.ko, test /dev/rico against rico_hostd
make driver-test-iouring  # same, host I/O served through io_uring (needs liburing)
```

The `io_uring` backend is optional: install `liburing-dev` (found via
`pkg-config`) or set `LIBURING_INC=` / `LIBURING_LIB=`. Without it everything else
still builds and `--backing` is reported as unavailable.

**Run the pieces by hand** with your own guest image:

```sh
# host: create/initialise the shared region and start polling (before QEMU!)
./build/rico_hostd /dev/shm/rico --busy --cpu 4 &

# host: boot a guest with the region attached as an ivshmem-plain device
./scripts/run_qemu.sh guest.qcow2

# guest (root): find the device, then run the stub against its BAR2
lspci -d 1af4:1110                                   # e.g. 00:04.0
./rico_guest /sys/bus/pci/devices/0000:00:04.0/resource2 100000 --pingpong --cpu 1
```

`rico_guest` is statically linked, so it can be copied into any guest.
`make integration` does all of this automatically with a tiny initramfs that
boots the host's own kernel (`scripts/run_integration_test.sh`; knobs: `N`,
`HOST_CPU`, `GUEST_CPU`, `KERNEL`, `QEMU_ACCEL`, `KEEP`).

Useful flags: `--pingpong` (depth 1, unloaded latency), `--depth D`, `--cpu N`,
`--warmup W` on the guest; `--cpu N`, `--busy` on the daemon.

### Guest kernel driver (`/dev/rico`)

`driver/rico.ko` binds the ivshmem device (`1af4:1110`), maps BAR2 and creates
`/dev/rico`. There is still no doorbell or interrupt: waiters busy-poll briefly,
then sleep while a `rico-poll` kthread reaps the CQ and wakes them.

```c
int fd = open("/dev/rico", O_RDWR);
struct rico_sqe s = { .user_data = 1, .op = RICO_OP_READ, .len = 16, .offset = 4096 };
write(fd, &s, sizeof s);                       /* array of rico_sqe: batched submit */
struct rico_wait w = { .min_complete = 1 };
ioctl(fd, RICO_IOC_WAIT, &w);                  /* or poll(2), or a blocking read(2) */
struct rico_cqe c; read(fd, &c, sizeof c);     /* array of rico_cqe */
```

See `driver/rico_uapi.h`. In-flight requests plus unread completions are capped at
the SQ depth (1024), so neither ring can overflow; a full `write` returns a partial
count or `EAGAIN`. The driver treats the host as untrusted: peer indices are
range-checked, completion ids carry a generation and are matched against an
in-flight table, and a corrupted ring disables the device (`EIO`).

Build it against the kernel the guest boots (vermagic must match):

```sh
make driver KDIR=/lib/modules/$(uname -r)/build
# in the guest:
insmod rico.ko && ls -l /dev/rico && rmmod rico
```

`make driver-test` automates this (knobs: `N`, `DEPTH`, `KERNEL`, `KDIR`,
`HOSTD_OPTS`, `KEEP`). It checks errno/ioctl behaviour, backpressure, abandoned
requests, a verified N-request workload, clean `rmmod`, and the host's counters.

### Host `io_uring` backend

By default `rico_hostd` answers from an in-memory mock device. With `--backing` it
serves requests from a real file or block device through `io_uring`:

```sh
./build/rico_hostd /dev/shm/rico --busy --backing /path/to/file_or_blockdev [--qd 256] [--sqpoll]
```

READ/WRITE become `IORING_OP_READ/WRITE` (registered buffers and file by default;
`--no-fixed` to disable), FLUSH becomes `IORING_OP_FSYNC` issued once every earlier
request has completed. Overlapping requests execute in submission order; others may
complete out of order (the guest matches completions by id). A WRITE completion
means "accepted by the page cache"; FLUSH is the durability point. Use
`--no-verify` when the client picks its own request ids (as `rico.ko` does).
Test it with `make driver-test-iouring` or `BACKING=iouring ./scripts/run_integration_test.sh`.

## Performance highlights

Single machine (i9-13900, KVM, host and guest pinned to different physical cores),
unloaded depth-1 ping-pong. Cycle counts are TSC cycles (`lfence; rdtsc`); the
nanosecond figures use a 20 ms TSC calibration (~2.0 GHz) and are approximate.
Raw logs: [docs/evaluation/](docs/evaluation/README.md).

| Scenario | p50 | p99 | Notes |
|---|---|---|---|
| **Guest in a QEMU/KVM VM** (3 runs × 190k samples) | **301–328 ns** (~600–650 cycles) | 365–380 ns | the headline number |
| Host-native, two threads | 308 ns | 368 ns | same code, no VM |
| Host-native, two processes | 313 ns | 383 ns | |
| Loaded throughput (host-native) | — | — | **7.1 Mreq/s** round-trip; latency then includes queueing (p50 ~390 ns) |

The in-VM latency is essentially the host-native latency, i.e. crossing the
VM boundary through the shared region costs no measurable extra. Caveats: these
are plain KVM guests (not confidential), and tail outliers (max ≫ p99.9) are
scheduler/interrupt noise that pinning reduces but does not remove.

## Limitations

- Not run on real SEV-SNP/TDX hardware; see the [threat model](docs/threat-model.md)
  for what that changes (notably: shared pages are readable by the host).
- "Exitless" is by construction (no notification mechanism exists); it hasn't
  been confirmed by counting `kvm_exit` events (no `perf`/tracefs access here).
- Storage is a mock device or a single file/block device via `io_uring`; no
  multi-device, multi-queue or QEMU-integrated block device yet. The `io_uring`
  path has been verified for correctness, but its performance is not yet in the
  [evaluation](docs/evaluation/README.md) headline numbers (those use the mock device).
- The kernel driver is a single-instance, unsigned out-of-tree module with an
  UC-mapped BAR (the same mapping type as `resource2`); it has been tested on one
  kernel (Debian 6.12) in a plain KVM guest only.
- Both sides busy-poll; an idle guest/host pair burns cores (the daemon backs
  off with sleeps unless `--busy`).
- No guest-side timeout or recovery if the host stops completing requests.
- The host worker is a standalone daemon, not (yet) a QEMU device; the worker
  API (`host/rico_host.h`) is written to drop into one.

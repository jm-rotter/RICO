# Architecture

## Components

| Component | File | Role |
|---|---|---|
| Rings | `src/rico_ring.h` | `RICO_DEFINE_RING` template, instantiated as SQ (`rico_ring`, entries `rico_req`) and CQ (`rico_cq`, entries `rico_cpl`) |
| Region layout | `src/rico_shm.h` | header (magic, version, geometry, `host_ready`) + SQ + CQ, padded to 256 KiB; also the deterministic mock workload |
| Host worker | `host/rico_host.{h,c}` | maps the file, polls SQ, validates, calls a handler, pushes CQ; optional CPU pin, busy-poll |
| Host daemon | `host/rico_hostd.c` | creates + initialises the file, runs the worker over `rico_mockdev.h` |
| Guest loop | `guest/rico_guest.h` | submit/complete loop with depth control, in-order verification, TSC stamping |
| Guest stub | `guest/rico_guest.c` | maps BAR2 (or any file), validates header, runs the loop |
| Test VM init | `guest/rico_init.c` | PID 1 of the integration initramfs |

## Shared region

```
offset 0       header   magic "RIC0", version, slot counts, entry sizes, host_ready
               SQ       head (line) | tail (line) | 1024 × rico_req  (64 B)
               CQ       head (line) | tail (line) | 1024 × rico_cpl  (64 B)
size 256 KiB   (ivshmem requires a power of two)
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

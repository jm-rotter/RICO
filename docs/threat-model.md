# Threat model: zero-trust CVM channel (SEV-SNP / TDX)

> Scope note: the code has only been run in a plain KVM guest. This document
> states the model RICO is designed for and what is and isn't implemented.

## Assumptions

- **Guest (CVM):** trusted by its owner. Its private memory is encrypted and
  integrity-protected by the platform (SEV-SNP RMP / TDX). The guest kernel and
  its userspace are the TCB.
- **Host / hypervisor / QEMU / `rico_hostd`:** **untrusted by the guest**
  (adversarial), and the guest is **untrusted by the host process** (a
  compromised guest must not corrupt the host).
- **The shared region** must live in memory the guest marks *shared*
  (unencrypted). Anything placed there is readable and writable by the host at
  any time, and may change while the guest reads it. Expected (not verified here)
  to hold for an ivshmem BAR, which is MMIO-type shared memory.

## Assets and what the channel does about them

| # | Threat | Mitigation in RICO | Status |
|---|---|---|---|
| 1 | Host reads channel contents | None: shared memory is plaintext to the host | **Not mitigated.** Payloads must be encrypted/authenticated above this layer (e.g. AEAD with a guest-held key) before sensitive data is sent |
| 2 | Host rewrites slots/indices at any moment (TOCTOU) | Consumers copy each slot exactly once and validate the *copy*; no in-place use | Implemented, both directions |
| 3 | Host sets `head`/`tail` to impossible values | `tail - head > N` is treated as empty/corrupt (`*_corrupt()`); guest aborts the run; ring sizes are compile-time constants, not read from shared memory | Implemented |
| 4 | Host forges/skips/reorders/replays completions | Guest checks `id` equals the next expected sequence number, `status`, `len ≤ max` | Implemented (sequence integrity; not content integrity, see 5) |
| 5 | Host returns wrong READ data | Guest *test* verifies against a model; a real driver has no model | **Not mitigated** — needs integrity protection (MAC/AEAD, or a Merkle scheme) at a higher layer |
| 6 | Host never completes (DoS) | None | **Not mitigated**: guest spins forever; needs a timeout/watchdog policy. DoS is generally out of scope for CVM threat models but should be bounded |
| 7 | Compromised guest sends malformed requests to harm the host | Host validates `op`, `len`, and (in the mock device) `offset+len` against device size; bad requests get `RICO_ST_INVAL`; fixed-size copies, no pointers cross the boundary | Implemented for the mock device; **real backends must re-validate** |
| 8 | Guest floods the host | Ring depth bounds queued work; host is single-threaded per ring | Partial (no rate limiting) |
| 9 | Info leak from host to guest via uninitialised bytes | Completion is zeroed before the handler fills it | Implemented |
| 10 | Info leak guest → host via struct padding | Requests are fully built (`memset`) before push | Implemented in the stub; any driver must do likewise |
| 11 | Host forges timestamps to skew results | Out-of-range deltas rejected; timestamps never drive decisions | Telemetry only |
| 12 | Host observes metadata (timing, sizes, offsets, op mix) via polling | None | **Not mitigated** (inherent to a host-visible ring; padding/batching/cover traffic are options) |
| 13 | Host rewrites the region header after attach | Guest reads the header once at attach; geometry is compile-time | Implemented |
| 14 | Shared region mapped with weaker memory type (e.g. write-combining) breaks ordering | Stub uses the default (uncached) `resource2`; docs warn that `resource2_wc` needs an `sfence` before the `tail` store | Documented, not enforced |
| 15 | Malicious/duplicate attach to the region by other host processes | File permissions (`0600`) only | Minimal |

## Trust boundaries (data flow)

```
 guest private memory ──(copy once, validate)──► ring slot ─(host may read/modify)─► host process
 guest private memory ◄─(copy once, validate)──  ring slot ◄(host writes)───────────  host process
```

Nothing crosses by pointer; the only values the host-controlled side can
influence in the guest's control flow are `id`, `status` and `len` of a copied
completion, each range-checked before use.

## What would be needed for a real CVM deployment

1. Confirm the BAR2 pages are shared/decrypted in the guest and that the guest
   never places private data in them (page-state-change handling).
2. Authenticated encryption of payloads with guest-held keys, plus replay
   protection (the sequence number is a start but is host-visible and not MACed).
3. A guest watchdog for unresponsive hosts.
4. Re-validation in the real backend, and per-guest isolation if one daemon
   serves multiple guests.
5. Fuzzing of both consumers (`rico_host_run`, `rico_guest_run`) with an
   adversarial peer writing arbitrary bytes into the region.

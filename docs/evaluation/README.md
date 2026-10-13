# Evaluation

Environment (`env.txt`): 13th Gen Intel i9-13900, Linux 6.12.111, QEMU 10.0.13
with KVM. Host worker pinned to CPU 4, guest to CPU 2 (native) / vCPU 1 (VM);
CPU 2 and 4 are on different physical cores. TSC calibrated at ~2.00 GHz.
Reproduce with the commands below; raw output is in this directory.

| Log | Command |
|---|---|
| `native_pingpong_threads.log` | `build/rico_test 2000000 --pingpong --guest-cpu 2 --host-cpu 4` |
| `native_pingpong_procs.log` | same with `--procs` |
| `native_loaded.log` | `build/rico_test 20000000 --guest-cpu 2 --host-cpu 4` |
| `vm_pingpong_run{1,2,3}.log` | `N=200000 HOST_CPU=4 GUEST_CPU=1 scripts/run_integration_test.sh` |

## Results

Unloaded round-trip (depth 1; first 10k requests discarded as warmup):

| Run | min (cyc) | p50 (cyc / ns) | p99 (cyc / ns) | p99.9 (cyc) |
|---|---|---|---|---|
| Native, threads | 492 | 624 / 308 | 764 / 368 | 838 |
| Native, processes | 476 | 616 / 313 | 734 / 383 | 1232 |
| VM run 1 | 460 | 602 / 301 | 728 / 365 | 1058 |
| VM run 2 | 508 | 648 / 325 | 758 / 380 | 1012 |
| VM run 3 | 500 | 654 / 328 | 728 / 365 | 952 |

Loaded (unbounded depth, native): 7.11 Mreq/s; round-trip including queueing
p50 778 cycles, p99 1330 cycles, p99.9 40836 cycles. This is *not* a latency
figure; it contains time spent behind earlier requests.

Every run verified every request (in-order completion, READ contents against the
guest's model device, host-side workload check): 0 errors.

## Reading these numbers

- Cycles are authoritative; ns = cycles / calibrated TSC (approximate, ±a few %).
- The measurement floor includes two `lfence;rdtsc` (~25 cycles each).
- Max values (up to ~8M cycles in one VM run) are scheduler/interrupt outliers,
  not channel behaviour; use p99/p99.9. Isolating CPUs (`isolcpus`, `nohz_full`)
  would tighten the tail.
- The VM and native numbers are within run-to-run variation of each other. The
  absence of an in-VM penalty is plausible for RAM-backed shared memory but has
  not been explained (e.g. via the guest mapping's memory type) or checked by
  counting VM exits.
- Not measured: SEV-SNP/TDX guests, QEMU-embedded worker, real storage backend,
  multi-queue scaling.

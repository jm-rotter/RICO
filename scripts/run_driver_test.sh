#!/bin/bash
# End-to-end test of the guest kernel module (driver/rico.ko) across QEMU/KVM.
#
#   host:  rico_hostd  <-->  /dev/shm/rico_drv.$$  <-->  QEMU ivshmem-plain
#   guest: tiny initramfs; rico_init (PID 1) insmods /rico.ko, waits for
#          /dev/rico, runs /rico_drvtest on it, rmmods the module, powers off.
#
# Pass = all of:
#   * module loaded and bound the ivshmem device ("rico: bound ...")
#   * /dev/rico appeared
#   * rico_drvtest printed PASS and exited 0 (errno checks, backpressure,
#     orphaned requests, N-request verified workload, device stats)
#   * rmmod succeeded and the driver's remove path ran ("rico: unbound ...")
#   * no BUG/Oops/WARNING/lockdep splat on the guest console
#   * rico_hostd consumed/completed exactly N with no bad requests
#
# Env knobs:  N=20000 (>= 2000)  DEPTH=64  TIMEOUT=180
#             KERNEL=/boot/vmlinuz-$(uname -r)
#             KDIR=/lib/modules/$(uname -r)/build   (must match KERNEL!)
#             QEMU_ACCEL=kvm|tcg   KEEP=1 (keep temp dir/logs)
#             HOST_CPU=<cpu> (pin hostd)
#             BACKING=iouring [BACKING_PATH=...]  serve I/O via the io_uring backend
set -u
cd "$(dirname "$0")/.."

N=${N:-20000}
DEPTH=${DEPTH:-64}
TIMEOUT=${TIMEOUT:-180}
KERNEL=${KERNEL:-/boot/vmlinuz-$(uname -r)}
KDIR=${KDIR:-/lib/modules/$(uname -r)/build}
QEMU=${QEMU:-qemu-system-x86_64}
if [ -z "${QEMU_ACCEL:-}" ]; then
    if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then QEMU_ACCEL=kvm; else QEMU_ACCEL=tcg; fi
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/rico_drv.XXXXXX")
SHM=/dev/shm/rico_drv.$$
HOSTD_PID=""; QEMU_PID=""

cleanup() {
    [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null && { kill "$QEMU_PID"; sleep 0.5; kill -9 "$QEMU_PID" 2>/dev/null; }
    [ -n "$HOSTD_PID" ] && kill -0 "$HOSTD_PID" 2>/dev/null && { kill "$HOSTD_PID"; sleep 0.2; kill -9 "$HOSTD_PID" 2>/dev/null; }
    rm -f "$SHM" "$SHM.img"
    if [ "${KEEP:-0}" = 1 ]; then echo "kept: $TMP"; else rm -rf "$TMP"; fi
}
trap cleanup EXIT
trap 'echo "interrupted"; exit 130' INT TERM

fail() {
    echo "FAIL: $*"
    [ -f "$TMP/guest.log" ] && { echo "--- guest serial log (tail) ---"; tail -60 "$TMP/guest.log"; }
    [ -f "$TMP/hostd.log" ] && { echo "--- hostd log (tail) ---"; tail -5 "$TMP/hostd.log"; }
    exit 1
}

[ "$N" -ge 2000 ] || fail "N must be >= 2000 (the test reserves 1124 requests for fixed phases)"
command -v "$QEMU" >/dev/null || fail "$QEMU not found"
command -v cpio >/dev/null || fail "cpio not found"
[ -r "$KERNEL" ] || fail "kernel $KERNEL not readable (set KERNEL=...)"
[ -f "$KDIR/Makefile" ] || fail "no kernel build tree at KDIR=$KDIR (install linux-headers-$(uname -r) or set KDIR=)"

make -s host guest || fail "userspace build"
make -s -C driver KDIR="$KDIR" || fail "module build (KDIR=$KDIR)"
[ -f driver/rico.ko ] || fail "driver/rico.ko missing after build"

# The module only loads into the kernel it was built for. Best-effort check.
VERMAGIC=$(modinfo -F vermagic driver/rico.ko 2>/dev/null | awk '{print $1}')
KVER=$(file -b "$KERNEL" 2>/dev/null | sed -n 's/.*version \([^ ,]*\).*/\1/p')
if [ -n "$VERMAGIC" ] && [ -n "$KVER" ] && [ "$VERMAGIC" != "$KVER" ]; then
    fail "rico.ko is built for $VERMAGIC but $KERNEL is $KVER: set KDIR to a matching tree"
fi

# --- initramfs: /init, /rico.ko, /rico_drvtest ------------------------------
mkdir "$TMP/root" && mkdir "$TMP/root"/{proc,sys,dev}
cp build/rico_init "$TMP/root/init"
cp driver/rico.ko "$TMP/root/rico.ko"
cp build/rico_drvtest "$TMP/root/rico_drvtest"
(cd "$TMP/root" && find . | cpio -o -H newc --quiet | gzip -1) > "$TMP/initrd.gz" || fail "initramfs"

# --- 1. host daemon ---------------------------------------------------------
# --no-verify: the driver assigns its own request ids, so the host must not
# expect the 0,1,2... workload sequence (device semantics are still real).
HOSTD_ARGS=("$SHM" "$N" --busy --no-verify)
if [ "${BACKING:-}" = iouring ]; then
    HOSTD_ARGS+=(--backing "${BACKING_PATH:-$SHM.img}")
fi
[ -n "${HOST_CPU:-}" ] && HOSTD_ARGS+=(--cpu "$HOST_CPU")
./build/rico_hostd "${HOSTD_ARGS[@]}" > "$TMP/hostd.log" 2>&1 &
HOSTD_PID=$!
for _ in $(seq 100); do [ -s "$SHM" ] && break; sleep 0.05; done
[ -s "$SHM" ] || fail "rico_hostd did not create $SHM"
sleep 0.3
kill -0 "$HOSTD_PID" 2>/dev/null || { cat "$TMP/hostd.log"; fail "rico_hostd exited at startup"; }

# --- 2. QEMU (loglevel=7 so the driver's pr_info lines reach the serial log) -
if [ "$QEMU_ACCEL" = kvm ]; then ACCEL=(-enable-kvm -cpu host); else ACCEL=(-accel tcg -cpu max); fi
echo "qemu accel=$QEMU_ACCEL kernel=$KERNEL N=$N depth=$DEPTH module=$VERMAGIC"
timeout -k 5 "$TIMEOUT" "$QEMU" "${ACCEL[@]}" -smp 2 -m 512 \
    -kernel "$KERNEL" -initrd "$TMP/initrd.gz" \
    -append "console=ttyS0 panic=-1 loglevel=7 rico_ko=/rico.ko rico_prog=/rico_drvtest rico_args=$N+--depth+$DEPTH" \
    -object memory-backend-file,id=rico,share=on,mem-path="$SHM",size=256K \
    -device ivshmem-plain,memdev=rico,id=rico0 \
    -display none -serial file:"$TMP/guest.log" -monitor none -no-reboot \
    -nodefaults > "$TMP/qemu.log" 2>&1 &
QEMU_PID=$!

# --- 3. wait for the guest to power off -------------------------------------
wait "$QEMU_PID"; QRC=$?; QEMU_PID=""
[ "$QRC" = 124 ] && fail "timed out after ${TIMEOUT}s"
[ "$QRC" = 0 ] || fail "qemu exited with $QRC: $(head -5 "$TMP/qemu.log")"

# --- 4. assertions ----------------------------------------------------------
LOG="$TMP/guest.log"
need() { grep -q "$1" "$LOG" || fail "$2 (expected /$1/ in guest log)"; }

need 'rico_init: insmod /rico.ko: ok'    "module did not load"
need 'rico: bound '                      "driver did not bind the ivshmem device"
need 'rico_init: /dev/rico present'      "/dev/rico was not created"
need 'drvtest: errno/ioctl checks done'  "errno checks did not complete"
need 'drvtest: backpressure checks done' "backpressure checks did not complete"
need "drvtest: $N requests, 0 errors"    "drvtest reported errors"
need 'drvtest: PASS'                     "drvtest did not pass"
need 'RICO_GUEST_EXIT=0'                 "drvtest exit status"
need 'RICO_RMMOD=0'                      "rmmod failed"
need 'rico: unbound '                    "driver remove path did not run"

if grep -E 'BUG:|Oops|WARNING:|possible (circular|recursive) locking|Call Trace|rico: shared ring corrupted' "$LOG" >/dev/null; then
    fail "kernel complained during the run"
fi
if grep -q 'drvtest: FAIL' "$LOG"; then fail "drvtest FAIL lines present"; fi

for _ in $(seq 40); do kill -0 "$HOSTD_PID" 2>/dev/null || break; sleep 0.1; done
if kill -0 "$HOSTD_PID" 2>/dev/null; then fail "rico_hostd still running (host saw fewer than $N completions)"; fi
wait "$HOSTD_PID"; HRC=$?; HOSTD_PID=""
grep -q "consumed=$N completed=$N bad=0 errors=0" "$TMP/hostd.log" \
    || fail "hostd stats wrong: $(grep consumed= "$TMP/hostd.log" | tail -1)"
[ "$HRC" = 0 ] || fail "rico_hostd exit $HRC"

echo "--- guest output ---"
grep -E 'rico:|rico_init:|drvtest:|RICO_' "$LOG"
echo "--- hostd ---"; grep consumed= "$TMP/hostd.log" | tail -1
echo "PASS: rico.ko loaded, $N requests through /dev/rico verified, module unloaded cleanly"

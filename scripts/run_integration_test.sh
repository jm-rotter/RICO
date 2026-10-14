#!/bin/bash
# End-to-end RICO test across a real QEMU/KVM boundary.
#
#   host:  rico_hostd  <-->  /dev/shm/rico_test.$$  <-->  QEMU ivshmem-plain
#   guest: tiny initramfs (rico_init as PID 1) boots the HOST's own kernel,
#          finds the ivshmem BAR2 in sysfs and runs rico_guest --pingpong on it.
#
# Pass = guest exits 0 with "N completed, 0 errors" AND hostd consumed/completed
# exactly N with no bad requests or workload mismatches.
#
# Env knobs:  N=10000  TIMEOUT=180  KERNEL=/boot/vmlinuz-$(uname -r)
#             HOST_CPU=<cpu> (pin hostd)   GUEST_CPU=<vcpu index> (pin in guest)
#             QEMU_ACCEL=kvm|tcg   KEEP=1 (keep temp dir/logs)
#             BACKING=iouring   serve I/O from a real file via the io_uring backend
#                               (needs liburing); BACKING_PATH=<file|blockdev> to
#                               choose the target (default: tmpfs file); HOSTD_OPTS="--sqpoll ..."
set -u
cd "$(dirname "$0")/.."

N=${N:-10000}
TIMEOUT=${TIMEOUT:-180}
KERNEL=${KERNEL:-/boot/vmlinuz-$(uname -r)}
QEMU=${QEMU:-qemu-system-x86_64}
if [ -z "${QEMU_ACCEL:-}" ]; then
    if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then QEMU_ACCEL=kvm; else QEMU_ACCEL=tcg; fi
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/rico_it.XXXXXX")
SHM=/dev/shm/rico_test.$$
HOSTD_PID=""; QEMU_PID=""

cleanup() {
    [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null && { kill "$QEMU_PID"; sleep 0.5; kill -9 "$QEMU_PID" 2>/dev/null; }
    [ -n "$HOSTD_PID" ] && kill -0 "$HOSTD_PID" 2>/dev/null && { kill "$HOSTD_PID"; sleep 0.2; kill -9 "$HOSTD_PID" 2>/dev/null; }
    rm -f "$SHM" "$SHM.img"
    if [ "${KEEP:-0}" = 1 ]; then echo "kept: $TMP"; else rm -rf "$TMP"; fi
}
trap cleanup EXIT
trap 'echo "interrupted"; exit 130' INT TERM

fail() { echo "FAIL: $*"; [ -f "$TMP/guest.log" ] && { echo "--- guest serial log (tail) ---"; tail -30 "$TMP/guest.log"; }; exit 1; }

command -v "$QEMU" >/dev/null || fail "$QEMU not found"
[ -r "$KERNEL" ] || fail "kernel $KERNEL not readable (set KERNEL=...)"
make -s host guest || fail "build"

# --- initramfs: just /init and /rico_guest (both static) --------------------
mkdir "$TMP/root" && mkdir "$TMP/root"/{proc,sys,dev}
cp build/rico_init "$TMP/root/init"; cp build/rico_guest "$TMP/root/rico_guest"
(cd "$TMP/root" && find . | cpio -o -H newc --quiet | gzip -1) > "$TMP/initrd.gz" || fail "initramfs"

# --- 1. host daemon ---------------------------------------------------------
HOSTD_ARGS=("$SHM" "$N" --busy)
if [ "${BACKING:-}" = iouring ]; then
    # tmpfs default: the test workload is 1/8 FLUSH, which on a real disk is an fsync
    # storm (ms-scale p90). Point BACKING_PATH at a disk/blockdev to see that cost.
    HOSTD_ARGS+=(--backing "${BACKING_PATH:-$SHM.img}")
fi
# shellcheck disable=SC2206
HOSTD_ARGS+=(${HOSTD_OPTS:-})
[ -n "${HOST_CPU:-}" ] && HOSTD_ARGS+=(--cpu "$HOST_CPU")
./build/rico_hostd "${HOSTD_ARGS[@]}" > "$TMP/hostd.log" 2>&1 &
HOSTD_PID=$!
for _ in $(seq 100); do [ -s "$SHM" ] && break; sleep 0.05; done
[ -s "$SHM" ] || fail "rico_hostd did not create $SHM"

# --- 2. QEMU ----------------------------------------------------------------
ARGS="$N+--pingpong"
[ -n "${GUEST_CPU:-}" ] && ARGS+="+--cpu+$GUEST_CPU"
if [ "$QEMU_ACCEL" = kvm ]; then ACCEL=(-enable-kvm -cpu host); else ACCEL=(-accel tcg -cpu max); fi
echo "qemu accel=$QEMU_ACCEL kernel=$KERNEL N=$N"
timeout -k 5 "$TIMEOUT" "$QEMU" "${ACCEL[@]}" -smp 2 -m 512 \
    -kernel "$KERNEL" -initrd "$TMP/initrd.gz" \
    -append "console=ttyS0 panic=-1 loglevel=3 rico_args=$ARGS" \
    -object memory-backend-file,id=rico,share=on,mem-path="$SHM",size=256K \
    -device ivshmem-plain,memdev=rico,id=rico0 \
    -display none -serial file:"$TMP/guest.log" -monitor none -no-reboot \
    -nodefaults > "$TMP/qemu.log" 2>&1 &
QEMU_PID=$!

# --- 3. wait for QEMU to exit (guest powers off when done) ------------------
wait "$QEMU_PID"; QRC=$?; QEMU_PID=""
[ "$QRC" = 124 ] && fail "timed out after ${TIMEOUT}s"
[ "$QRC" = 0 ] || fail "qemu exited with $QRC: $(head -5 "$TMP/qemu.log")"

# --- 4. assertions ----------------------------------------------------------
GEXIT=$(sed -n 's/.*RICO_GUEST_EXIT=\([0-9]*\).*/\1/p' "$TMP/guest.log" | tail -1)
[ "$GEXIT" = 0 ] || fail "guest exit status '${GEXIT:-missing}'"
grep -q "guest: $N submitted, $N completed, 0 errors" "$TMP/guest.log" \
    || fail "guest summary line missing / nonzero errors"

# hostd exits by itself once N completions are posted; give it a moment.
for _ in $(seq 40); do kill -0 "$HOSTD_PID" 2>/dev/null || break; sleep 0.1; done
if kill -0 "$HOSTD_PID" 2>/dev/null; then fail "rico_hostd still running (host saw fewer than $N completions)"; fi
wait "$HOSTD_PID"; HRC=$?; HOSTD_PID=""
grep -q "consumed=$N completed=$N bad=0 errors=0" "$TMP/hostd.log" \
    || fail "hostd stats wrong: $(tail -1 "$TMP/hostd.log")"
[ "$HRC" = 0 ] || fail "rico_hostd exit $HRC"

echo "--- guest output ---"
sed -n '/rico_init: using/,/RICO_GUEST_EXIT/p' "$TMP/guest.log"
echo "--- hostd ---"; tail -1 "$TMP/hostd.log"
echo "PASS: $N ping-pong requests across the QEMU boundary, 0 errors"

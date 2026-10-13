#!/bin/sh
# Launch QEMU with the RICO region exposed to the guest as an ivshmem-plain
# PCI device (BAR2 = the shared file). Start rico_hostd first so the file exists.
#
#   ./build/rico_hostd /dev/shm/rico &         # creates + initialises the region
#   ./run_qemu.sh guest.qcow2            # boot any Linux guest image
#
# In the guest (as root):
#   lspci -d 1af4:1110                   # find the device, e.g. 00:04.0
#   ./build/rico_guest /sys/bus/pci/devices/0000:00:04.0/resource2 5000000
#
# share=on is required so guest writes land in the file the host maps.
SHM=${RICO_SHM:-/dev/shm/rico}
IMG=${1:?usage: run_qemu.sh <disk-image> [extra qemu args]}
shift
exec qemu-system-x86_64 \
    -enable-kvm -cpu host -smp 2 -m 2G \
    -drive file="$IMG",if=virtio \
    -object memory-backend-file,id=rico,share=on,mem-path="$SHM",size=256K \
    -device ivshmem-plain,memdev=rico,id=rico0 \
    -nographic "$@"

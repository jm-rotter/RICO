#!/bin/sh
# Host daemon + guest stub as unrelated processes over a shared file (no VM).
set -e
N=${1:-1000000}
SHM=${RICO_SHM:-/dev/shm/rico}
cd "$(dirname "$0")/.."
./build/rico_hostd "$SHM" "$N" &
HOST=$!
while [ ! -s "$SHM" ]; do sleep 0.05; done
sleep 0.1
./build/rico_guest "$SHM" "$N"
wait $HOST && echo "local host/guest test: PASS"
rm -f "$SHM"

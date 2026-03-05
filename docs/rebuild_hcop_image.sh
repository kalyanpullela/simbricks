#!/bin/bash
set -e
QEMU_NBD=sims/external/qemu/build/qemu-nbd
make -C images/hcop
mkdir -p images/output-hcop
cp images/output-base/base images/output-hcop/hcop
sudo modprobe nbd
sudo $QEMU_NBD -c /dev/nbd0 images/output-hcop/hcop
sudo mount /dev/nbd0p1 /mnt
sudo cp images/hcop/paxos_host images/hcop/lock_host images/hcop/barrier_host /mnt/usr/local/bin/
sudo chmod +x /mnt/usr/local/bin/*_host
echo "Installed binaries:"
ls -la /mnt/usr/local/bin/*_host
sudo umount /mnt
sudo $QEMU_NBD -d /dev/nbd0
echo "Done: images/output-hcop/hcop"

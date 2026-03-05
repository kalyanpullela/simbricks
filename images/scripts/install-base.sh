#!/bin/bash -eux

set -eux

export DEBIAN_FRONTEND=noninteractive

apt-get update || true
# Some minimal images lack universe repository which has sysbench, numactl, etc.
apt-get -y install software-properties-common || true
add-apt-repository -y universe || true
apt-get update || true

apt-get -y install \
    iperf \
    iputils-ping \
    netperf \
    netcat \
    ethtool \
    tcpdump \
    pciutils || true

pushd /tmp/input
mv guestinit.sh /home/ubuntu/guestinit.sh
mv bzImage /boot/vmlinuz-5.15.93
mv config-5.15.93 /boot/
mv m5 /sbin/m5

GRUB_CFG_FILE=/etc/default/grub.d/50-cloudimg-settings.cfg
echo 'GRUB_DISABLE_OS_PROBER=true' >> $GRUB_CFG_FILE
echo 'GRUB_HIDDEN_TIMEOUT=0' >> $GRUB_CFG_FILE
echo 'GRUB_TIMEOUT=0' >> $GRUB_CFG_FILE
update-grub

# with stupid ubuntu22 /lib is a symlink at which point just untaring to / will
# replace that symlink with a directory, so first extract and then carefully
# copy... -.-
mkdir kheaders
cd kheaders
tar xzf /tmp/input/kheaders.tar.gz
cp -a lib/modules/* /lib/modules/
cp -a usr/* /usr/

# cleanup
popd
rm -rf /tmp/input

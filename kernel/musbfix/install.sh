#!/bin/sh
# Build, install and enable musbfix to auto-load at boot.
# Run on the target board (needs kernel headers, gcc, make, wget, sudo).
set -e

KREL="$(uname -r)"
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

echo ">> building musbfix for $KREL"
make

echo ">> installing to /lib/modules/$KREL/extra/"
sudo install -D -m 0644 musbfix.ko "/lib/modules/$KREL/extra/musbfix.ko"
sudo depmod -a

echo ">> enabling auto-load at boot (/etc/modules-load.d/musbfix.conf)"
echo musbfix | sudo tee /etc/modules-load.d/musbfix.conf >/dev/null

echo ">> loading now"
sudo modprobe -r musbfix 2>/dev/null || true
sudo modprobe musbfix

echo ">> done. dmesg:"
sudo dmesg | grep -i musbfix | tail -3 || true
echo
echo "To remove:  sudo modprobe -r musbfix && sudo rm -f /etc/modules-load.d/musbfix.conf \\"
echo "            /lib/modules/$KREL/extra/musbfix.ko && sudo depmod -a"

#!/bin/sh
# Headless setup for usb-proxy on an Orange Pi Zero (sunxi musb).
# Run this ON the board, from a checked-out copy of this repo (opi branch).
#
# It will:
#   1. build usb-proxy
#   2. build + install the musbfix kernel module and enable it at boot
#   3. install raw_gadget into extra/ (custom .ko if provided) and load at boot
#   4. disable g_serial (it competes for the single musb UDC)
#   5. install + enable the usb-proxy systemd service and udev rule
#
# Env vars:
#   RAW_GADGET_KO=/path/to/raw_gadget.ko   use a custom raw_gadget build
#                                          (otherwise the in-kernel module is used)
#   PROXY_DIR=/home/darrel/usb-proxy       where usb-proxy is built/run from
#
# See docs/orange-pi-headless-setup.md for the full explanation.
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
KREL="$(uname -r)"
PROXY_DIR="${PROXY_DIR:-$REPO}"

echo "== repo:        $REPO"
echo "== kernel:      $KREL"
echo "== proxy dir:   $PROXY_DIR"

echo "== 1/5 build usb-proxy"
make -C "$REPO"

echo "== 2/5 build + install musbfix (auto-load at boot)"
( cd "$REPO/kernel/musbfix" && make )
sudo install -D -m0644 "$REPO/kernel/musbfix/musbfix.ko" "/lib/modules/$KREL/extra/musbfix.ko"
echo musbfix | sudo tee /etc/modules-load.d/musbfix.conf >/dev/null

echo "== 3/5 install raw_gadget (auto-load at boot)"
if [ -n "$RAW_GADGET_KO" ]; then
	echo "   using custom raw_gadget: $RAW_GADGET_KO"
	sudo install -D -m0644 "$RAW_GADGET_KO" "/lib/modules/$KREL/extra/raw_gadget.ko"
else
	echo "   using in-kernel raw_gadget (set RAW_GADGET_KO to override)"
fi
echo raw_gadget | sudo tee /etc/modules-load.d/raw_gadget.conf >/dev/null
sudo depmod -a
# Retire any hand-rolled raw_gadget loader service.
sudo systemctl disable raw-gadget.service 2>/dev/null || true

echo "== 4/5 disable g_serial (conflicts with raw_gadget for the single UDC)"
if grep -qE '^[[:space:]]*g_serial' /etc/modules 2>/dev/null; then
	sudo sed -i 's/^[[:space:]]*g_serial.*/#&  # disabled: conflicts with raw_gadget for the musb UDC/' /etc/modules
fi

echo "== 5/6 reduce SD writes (logs/swap already on zram via armbian-ramlog)"
sudo systemctl disable --now apt-daily.timer apt-daily-upgrade.timer 2>/dev/null || true
sudo systemctl disable --now rsyslog.service 2>/dev/null || true   # redundant with journald

echo "== 6/6 install + enable service and udev rule"
# Point the unit at PROXY_DIR if it differs from the default.
sed "s#/home/darrel/usb-proxy#$PROXY_DIR#g" "$REPO/deploy/usb-proxy.service" \
	| sudo tee /etc/systemd/system/usb-proxy.service >/dev/null
sudo install -m0644 "$REPO/deploy/99-usb-proxy.rules" /etc/udev/rules.d/99-usb-proxy.rules
sudo systemctl daemon-reload
sudo udevadm control --reload-rules
sudo systemctl enable usb-proxy.service

echo
echo "Done. Load modules + start now with:"
echo "  sudo modprobe raw_gadget musbfix && sudo systemctl restart usb-proxy.service"
echo "Then plug in a device and check:  systemctl status usb-proxy.service"
echo "On the host PC:  adb devices   (or  fastboot devices)"

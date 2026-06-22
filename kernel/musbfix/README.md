# musbfix — runtime fix for sunxi musb bulk-OUT packet loss

A small loadable kernel module that fixes a USB gadget bug on Allwinner **sunxi
musb** controllers (e.g. Orange Pi Zero, H2+/sun8i-h3) which otherwise breaks
`usb-proxy`: an Android device enumerates but stays **`offline`**, the log fills
with `Transfer error receiving on EP81: Operation timed out`, and the host's ADB
CNXN payload is never delivered.

The same `usb-proxy` setup works on dwc2/dwc3 boards (e.g. Raspberry Pi) — the
bug is specific to the sunxi musb peripheral controller.

## Root cause

`musb_ep_restart()` (`drivers/usb/musb/musb_gadget.c`), called from
`musb_gadget_queue()` when a request becomes the head of the queue, **flushes
the RX FIFO** for OUT endpoints and never services a packet already sitting in
it. The sunxi musb is **PIO-only and single-buffered**, so when a packet arrives
during the brief window where no request is queued — which happens on every
transfer with `raw-gadget`, since it re-queues OUT reads from user space — the
controller ACKs it into the single FIFO and the next `usb_ep_queue()` flushes it
away. The host considers the packet delivered and never resends → deadlock.

In-kernel gadgets re-queue OUT requests from completion (IRQ) context with no
user-space round-trip, so the window is ~zero and they don't hit this.
`raw-gadget` does, which is why `usb-proxy` exposes it.

See the full write-up in the header comment of [`musbfix.c`](musbfix.c).

## What the module does

`arm32` has no ftrace instruction-pointer modification (no live patching), so
the module places a **kprobe** at `musb_ep_restart` and **always redirects**
execution (never single-steps the Thumb-2 entry, which corrupts the kernel on
this platform):

- **OUT (rx):** run `musb_g_rx()`, which services any already-pending FIFO
  packet (or leaves the request armed) instead of flushing it.
- **IN (tx):** replicate the original path (`musb_ep_select` + `txstate`), so IN
  transfers are unaffected.

It is fully reversible (`rmmod`) and never modifies the on-disk kernel.

## Build & install (on the target board)

Requires the matching kernel headers (`linux-headers-*`), `gcc`, `make`,
`wget`, and `sudo`.

```sh
./install.sh        # build, install to extra/, depmod, enable auto-load, load now
```

Or manually:

```sh
make                # fetches matching musb headers, builds musbfix.ko
sudo insmod musbfix.ko
dmesg | tail        # "musbfix: kprobe musb_ep_restart@... (rx->musb_g_rx@... tx->txstate@...)"
```

To make it load on every boot (what `install.sh` does):

```sh
sudo install -D -m0644 musbfix.ko /lib/modules/$(uname -r)/extra/musbfix.ko
sudo depmod -a
echo musbfix | sudo tee /etc/modules-load.d/musbfix.conf
```

Remove:

```sh
sudo modprobe -r musbfix
sudo rm -f /etc/modules-load.d/musbfix.conf /lib/modules/$(uname -r)/extra/musbfix.ko
sudo depmod -a
```

## Verifying the fix

Run `usb-proxy` with an Android device attached and, on the USB host, `adb
devices`. With the module loaded you should see the device as `device` (not
`offline`), and the `usb-proxy` log shows OUT reads of varied sizes plus
`EP81(bulk_in): wrote N bytes to host` (real ADB OPEN/OKAY/WRTE/CLSE traffic).

## Caveats

- **arm32 Thumb-2 specific.** It handles the PC Thumb bit explicitly.
- **Tied to the running kernel build.** It includes internal `musb` headers
  (fetched by `make` for the running kernel's base version) and is compiled
  against the kernel's own `.config`, so struct offsets match. It is also tied
  to the kernel's `vermagic`: **after a kernel update it must be rebuilt and
  retested** — until then it simply won't load and the bug returns. (For
  rebuild-on-update, package it with DKMS.)
- Worst case on a mismatch is an oops that kills `usb-proxy`; the on-disk kernel
  is untouched, so a reboot fully recovers.

## Proper fix

The correct fix belongs in the kernel: service the pending RX packet instead of
flushing it. See [`musb_gadget-rx-requeue.patch`](musb_gadget-rx-requeue.patch),
suitable for building a patched kernel or submitting upstream
(`linux-usb` / `linux-sunxi`).

## Tested on

Orange Pi Zero (H2+/sun8i-h3), Armbian, kernel `6.18.35-current-sunxi`.

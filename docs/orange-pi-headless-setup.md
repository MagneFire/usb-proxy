# Running usb-proxy headlessly on an Orange Pi Zero (sunxi musb)

This guide documents everything needed to run `usb-proxy` reliably and headlessly
on an **Orange Pi Zero** (Allwinner **H2+ / sun8i-h3**, `musb-hdrc` USB
controller), proxying Android devices (ADB, fastboot, …) between a host PC and
the device.

The sunxi musb peripheral controller has several quirks that break `usb-proxy`
out of the box (the device shows `offline`, fastboot devices get reset off the
bus, etc.). This branch (`opi`) contains the source fixes; this document
explains them and the full headless deployment. The same setup works unmodified
on a dwc2-based board (e.g. Raspberry Pi), which is why those quirks weren't seen
there.

> **Tested on:** Orange Pi Zero (H2+/sun8i-h3), Armbian, kernel
> `6.18.35-current-sunxi` (arm32, Thumb-2).

## Topology

```
   Host PC (USB host, runs adb/fastboot)
        │  USB  (Orange Pi OTG port = musb gadget, "musb-hdrc.2.auto")
        ▼
   Orange Pi Zero ── usb-proxy (raw-gadget) ──┐
        ▲                                      │
        │  USB  (Orange Pi USB-A port = ehci host)
        ▼
   Android device (the proxied device)
```

`usb-proxy` presents a gadget to the PC (via `raw-gadget` on the musb UDC) and
talks to the real device as a USB host (via `libusb`).

## What was broken on musb, and the fixes

| # | Symptom | Root cause | Fix |
|---|---------|-----------|-----|
| 1 | Device enumerates but ADB shows **`offline`**; host's follow-up bulk-OUT packet (e.g. ADB CNXN payload) is dropped | Kernel bug: `musb_ep_restart()` **flushes the RX FIFO** on every OUT re-queue; sunxi musb is single-buffered PIO, so a packet that arrived in the no-request window is discarded | **`musbfix` kernel module** (`kernel/musbfix/`) |
| 2 | `Transfer error receiving on EP81: Operation timed out` flood; stream corruption | `libusb_clear_halt` called on normal bulk-IN timeouts (resets data toggle); phantom zero-length packets forwarded | usb-proxy fix in `device-libusb.cpp` / `proxy.cpp` (commit *bulk-IN timeout handling*) |
| 3 | musb mishandles oversized OUT reads | OUT read buffer larger than one packet | Clamp OUT reads to `wMaxPacketSize` on musb (`gadget_is_musb`) |
| 4 | **fastboot** device gets reset off the bus despite `reset_device_before_proxy=false` | Event-loop reset on spurious musb disconnect events wasn't gated by the flag | usb-proxy fix: honor `reset_device_before_proxy` for reset/disconnect events too |
| 5 | At boot, `usb-proxy` doesn't start / gadget never appears | A single UDC, grabbed by `g_serial`; and the service wasn't enabled | Disable `g_serial`; enable the service |
| 6 | Worked for one device, not another; or device plugged before boot didn't start | udev rule matched one VID/PID | Generic udev rule + always-enabled service |

## Prerequisites

```sh
sudo apt update
sudo apt install -y build-essential pkg-config \
    libusb-1.0-0-dev libjsoncpp-dev \
    linux-headers-current-sunxi   # must match `uname -r`
# (kernel build for musbfix also needs: bc flex bison libssl-dev — usually present)
```

Kernel must have (Armbian `current` does): `CONFIG_USB_RAW_GADGET=m`,
`CONFIG_KPROBES=y`, `CONFIG_DYNAMIC_FTRACE_WITH_REGS=y`. `CONFIG_USB_MUSB_HDRC=y`
means the musb driver is **built into the kernel image** (relevant for `musbfix`,
which patches it at runtime rather than swapping a module).

## Quick path (automated)

From a checkout of this repo (`opi` branch) on the board:

```sh
# optional: use a custom raw_gadget build instead of the in-kernel one
RAW_GADGET_KO=/home/darrel/raw-gadget/raw_gadget/raw_gadget.ko \
  ./deploy/setup.sh
sudo modprobe raw_gadget musbfix
sudo systemctl restart usb-proxy.service
```

Then plug in a device and check `adb devices` / `fastboot devices` on the host.
The sections below explain each step that `setup.sh` performs.

## Manual steps

### 1. Build usb-proxy

```sh
make            # produces ./usb-proxy
```

### 2. raw_gadget module

`usb-proxy` needs the `raw_gadget` driver. Install the `.ko` into `extra/` and
load it at boot via `modules-load.d` (cleaner and more standard than a custom
`insmod` service; `extra/` also takes precedence over the in-kernel copy so a
custom build wins):

```sh
# Custom build (recommended if you use one), or skip to use the in-kernel module:
sudo install -D -m0644 /home/darrel/raw-gadget/raw_gadget/raw_gadget.ko \
    /lib/modules/$(uname -r)/extra/raw_gadget.ko
echo raw_gadget | sudo tee /etc/modules-load.d/raw_gadget.conf
sudo depmod -a
sudo systemctl disable raw-gadget.service 2>/dev/null   # retire any old loader service
```

### 3. musbfix kernel module (the bulk-OUT fix)

Without this, OUT-heavy protocols (the ADB CNXN handshake) stall and the device
shows `offline`. Build and install it (auto-loads at boot):

```sh
cd kernel/musbfix
./install.sh        # build, install to extra/, depmod, modules-load.d, load now
```

See [`kernel/musbfix/README.md`](../kernel/musbfix/README.md) for the full
explanation and the equivalent in-tree kernel patch
(`kernel/musbfix/musb_gadget-rx-requeue.patch`).

> **Kernel-update caveat:** `musbfix` is tied to the exact kernel build
> (vermagic + internal struct offsets). After an Armbian kernel upgrade it must
> be **rebuilt and retested** — until then it won't load and bug #1 returns.

### 4. Disable g_serial

The board has a **single** musb UDC. If `g_serial` (listed in `/etc/modules`)
loads at boot it grabs the UDC and `usb-proxy` can't bind it:

```sh
sudo sed -i 's/^g_serial/#g_serial/' /etc/modules
```

### 5. systemd service

Install [`deploy/usb-proxy.service`](../deploy/usb-proxy.service) to
`/etc/systemd/system/`, adjusting `WorkingDirectory`/`ExecStart` paths if your
checkout isn't `/home/darrel/usb-proxy`:

```sh
sudo cp deploy/usb-proxy.service /etc/systemd/system/usb-proxy.service
sudo systemctl daemon-reload
sudo systemctl enable usb-proxy.service     # start at every boot
```

Key choices in the unit:
- **`WantedBy=multi-user.target`** — starts at boot regardless of what's
  plugged; `usb-proxy` then polls for *any* device (no VID/PID filter), so a
  device present at boot is picked up. This is what makes "plug in before
  power-on" work.
- **`Restart=always`** — self-heals on crash.
- **`ExecStartPre=-/sbin/modprobe …`** — belt-and-suspenders module load.
- **`stdbuf -oL`** — line-buffer stdout so the journal shows progress live.

### 6. udev rule (device plug/unplug)

Install [`deploy/99-usb-proxy.rules`](../deploy/99-usb-proxy.rules) so the
service reconnects when a device is (re)plugged and stops on unplug — for *any*
non-hub USB device:

```sh
sudo cp deploy/99-usb-proxy.rules /etc/udev/rules.d/99-usb-proxy.rules
sudo udevadm control --reload-rules
```

### 7. config.json — fastboot / no-reset devices

Some devices (e.g. anything in **fastboot mode**) drop off the bus on a USB
reset. Set `reset_device_before_proxy` to `false` in `config.json` (used because
the service runs with `--enable_customized_config`):

```json
{
    "reset_device_before_proxy": false
}
```

With fix #4, this now suppresses **all** device resets, including the ones the
event loop previously issued on musb's spurious disconnect events.

## Verifying

```sh
systemctl status usb-proxy.service          # active; "enabled"
cat /sys/class/udc/musb-hdrc.2.auto/state   # "configured" once a device is proxied
journalctl -u usb-proxy.service -f          # live log
```

On the **host PC**:
```sh
adb devices         # -> "device" (not "offline")
fastboot devices    # for a device in fastboot mode
```

Healthy log signs: `Device opened successfully`, `Setup USB config successfully`,
bulk reads of various sizes, and `EP81(bulk_in): wrote N bytes to host`. For a
no-reset device you should **not** see `Resetting device`.

End-to-end checks worth doing once:
1. Plug a device in while running → connects.
2. Unplug → service stops; replug (same or different device) → reconnects.
3. **Reboot with a device already attached** → service auto-starts and connects.

## Troubleshooting

- **Device shows `offline`, `EP81 ... timed out`, no second bulk-OUT read:**
  `musbfix` isn't loaded. `lsmod | grep musbfix`; `sudo modprobe musbfix`;
  `dmesg | grep musbfix`. After a kernel update, rebuild it (step 3).
- **fastboot device resets / fails to enumerate (`error -71`):** ensure
  `config.json` has `reset_device_before_proxy: false` and the binary includes
  fix #4 (`grep -c "Honour reset_device_before_proxy" proxy.cpp` → 1). Rebuild.
- **Service didn't start at boot:** confirm `systemctl is-enabled
  usb-proxy.service` is `enabled`; confirm `g_serial` isn't holding the UDC
  (`cat /sys/class/udc/musb-hdrc.2.auto/function`).
- **`modprobe raw_gadget` loads the wrong module:** `extra/` must contain your
  build and `depmod -a` must have run (`grep raw_gadget
  /lib/modules/$(uname -r)/modules.dep` should point at `extra/`).
- **Kernel oops after a kernel update:** `musbfix` offsets no longer match —
  `sudo rmmod musbfix` (or reboot; the on-disk kernel is untouched) and rebuild.

## File map

| Path | Purpose |
|------|---------|
| `kernel/musbfix/` | the bulk-OUT fix kernel module (+ README, in-tree patch) |
| `deploy/usb-proxy.service` | systemd unit (enabled, device-agnostic) |
| `deploy/99-usb-proxy.rules` | udev rule (any non-hub device → start/stop) |
| `deploy/setup.sh` | one-shot installer for all of the above |
| `proxy.cpp`, `device-libusb.cpp`, `usb-proxy.cpp` | the usb-proxy source fixes (#2–#4) |

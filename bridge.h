// Persistent gadget: the bulk bridge between the fixed gadget's adb/fastboot
// interfaces and whatever device is currently attached on the host side.
//
// The fixed gadget (gadget-fixed.cpp) is enumerated once and never rebuilt;
// its two bridge interfaces are host_device_desc.configs[0].interfaces[slot].
// Their endpoints are enabled for as long as the host keeps the configuration
// set, and forwarding threads (proxy.cpp ep_loop_read/ep_loop_write) run on a
// slot only while a device is bound to it. An unbound slot is HALTED: the
// host's pending transfers fail at once instead of hanging, adb drops its
// transport and re-opens it with a fresh CNXN (which then NAK-holds until a
// device binds), and fastboot commands fail fast. A bound slot is un-halted
// before its threads start.
//
// Two threads call in: the fixed gadget's ep0 thread (host configured /
// unconfigured, endpoint halt requests) and the device manager in main()
// (bind / unbind). Endpoint threads only ever call bridge_note_device_lost().
#ifndef BRIDGE_H
#define BRIDGE_H

#include <stdint.h>

enum {
	BRIDGE_SLOT_ADB = 0,
	BRIDGE_SLOT_FASTBOOT = 1,
	BRIDGE_NUM_SLOTS = 2,
};

// Build host_device_desc for the fixed gadget: one configuration, the two
// bridge interfaces (class ff/42, protocol 01 and 03, two bulk endpoints each,
// placeholder addresses the endpoint remapper replaces). Interface numbers are
// assigned by bridge_set_interface_base() once it is known whether the console
// takes 0/1.
void bridge_setup_desc(void);
void bridge_set_interface_base(uint8_t base);

// After the raw-gadget fd is running and endpoints are remapped.
void bridge_init(int fd);

// ep0 thread.
void bridge_host_configured(void);	// after USB_RAW_IOCTL_CONFIGURE
void bridge_host_unconfigured(void);	// SET_CONFIGURATION(0), reset, disconnect
// Endpoint-directed standard requests for a bridge endpoint (by gadget-side
// address). Returns false if the address is not a bridge endpoint.
bool bridge_ep_is_ours(uint8_t addr);
bool bridge_ep_halted(uint8_t addr);
void bridge_ep_set_halt(uint8_t addr, bool halt);
// Interface-directed: is this one of the bridge interfaces?
bool bridge_iface_is_ours(uint8_t iface);

// Device manager.
bool bridge_bind(int slot, uint8_t dev_in_addr, uint8_t dev_out_addr);
void bridge_unbind(void);
// Block until the bound device is reported lost, or please_stop_ep0, or
// timeout_ms (<0 = forever). Returns true if the device was lost.
bool bridge_wait_device_lost(int timeout_ms);
bool bridge_device_lost_pending(void);
// A new device has just been opened: forget the previous one's loss.
void bridge_clear_device_lost(void);

// Any thread: the bound device is gone. Idempotent, never blocks on the
// bridge state (it may be called from a thread the manager is about to join).
void bridge_note_device_lost(const char *where);

// The adb host->device stream, kept framed ACROSS device binds. A device can
// die mid-message (the watch's boot brings up a short-lived adb instance that
// swallows the head of the host's CNXN); the tail then arrives after the next
// bind and would reach the fresh adbd as garbage, wedging it. The bulk OUT
// read thread of the adb slot calls bridge_out_feed() on every read before
// forwarding: false = drop this read (a tail whose head went elsewhere, or
// bytes with no header in sight). It returns with the stream lock held, so
// the caller must call bridge_out_done() once the read is forwarded (or
// dropped) -- a replayed CNXN can then never land inside a host message.
bool bridge_out_feed(const uint8_t *data, int len);
void bridge_out_done(void);
// Device manager, shortly after a bind: if the host has not sent a CNXN since
// the bind (its transport is stale from the previous device), replay the last
// one it did send so the new adbd connects. No-op without a captured CNXN.
void bridge_replay_cnxn_if_needed(void);

#endif /* BRIDGE_H */

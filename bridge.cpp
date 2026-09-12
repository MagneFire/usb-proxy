#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <mutex>

#include <vector>

#include "bridge.h"
#include "device-libusb.h"
#include "host-raw-gadget.h"
#include "misc.h"
#include "proxy.h"

namespace {

// adb transport framing of the host->device stream (see bridge_out_feed).
struct OutStream {
	std::mutex mtx;			// held from bridge_out_feed to bridge_out_done
	uint32_t payload_remaining = 0;	// of the message whose header was seen
	bool dropping = false;		// the current message's head went to a dead device
	bool unsynced = false;		// no header where one was due: drop until one shows
	// Last complete host CNXN (header + payload), replayed to a device that
	// binds while the host's transport is stale.
	std::vector<uint8_t> cnxn;
	std::vector<uint8_t> cnxn_building;
	uint32_t cnxn_building_left = 0;
	bool cnxn_seen_since_bind = false;
};

OutStream g_out;

uint32_t le32(const uint8_t *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

// A_CNXN as adb encodes it ("CNXN" little-endian).
const uint32_t A_CNXN = 0x4e584e43;

bool parse_hdr(const uint8_t *p, uint32_t *cmd, uint32_t *len)
{
	*cmd = le32(p);
	*len = le32(p + 12);
	uint32_t magic = le32(p + 20);
	return magic == (*cmd ^ 0xffffffffU) && *len <= 1024 * 1024;
}

struct Bridge {
	std::mutex mtx;		// state below
	int fd = -1;
	bool configured = false;	// host has SET_CONFIGURATION(1) in effect
	int bound_slot = -1;		// device bound to this slot (or none)
	bool running = false;		// bound slot's threads are up
	bool halted[BRIDGE_NUM_SLOTS][2] = {};	// [slot][0=IN,1=OUT]

	// The loss flag has its own lock: endpoint threads set it while the
	// manager may hold mtx joining them.
	std::mutex lost_mtx;
	std::condition_variable lost_cv;
	bool lost = false;
};

Bridge g;

struct raw_gadget_altsetting *slot_alt(int slot)
{
	return &host_device_desc.configs[0].interfaces[slot].altsettings[0];
}

const char *slot_name(int slot)
{
	return slot == BRIDGE_SLOT_ADB ? "adb" : "fastboot";
}

// The IN (dir_in) or OUT endpoint of a slot.
struct raw_gadget_endpoint *slot_ep(int slot, bool dir_in)
{
	struct raw_gadget_altsetting *alt = slot_alt(slot);
	for (int k = 0; k < alt->interface.bNumEndpoints; k++)
		if ((bool)usb_endpoint_dir_in(&alt->endpoints[k].endpoint) == dir_in)
			return &alt->endpoints[k];
	return nullptr;
}

bool find_ep(uint8_t addr, int *slot, int *dir)
{
	for (int s = 0; s < BRIDGE_NUM_SLOTS; s++)
		for (int d = 0; d < 2; d++) {
			struct raw_gadget_endpoint *ep = slot_ep(s, d == 0);
			if (ep && ep->endpoint.bEndpointAddress == addr) {
				*slot = s;
				*dir = d;
				return true;
			}
		}
	return false;
}

// Caller holds g.mtx and the endpoints are enabled.
void set_halt_locked(int slot, bool halt)
{
	for (int d = 0; d < 2; d++) {
		struct raw_gadget_endpoint *ep = slot_ep(slot, d == 0);
		if (!ep || ep->thread_info.ep_num < 0)
			continue;
		int rv = halt ? usb_raw_ep_set_halt_try(g.fd, ep->thread_info.ep_num)
			      : usb_raw_ep_clear_halt_try(g.fd, ep->thread_info.ep_num);
		// Always logged: whether the UDC took the halt decides whether the
		// host's stale transfers fail (adb then re-opens its transport).
		printf("[%.3f] bridge: %s halt on EP%02x (#%d): %s\n", uptime_s(),
		       halt ? "set" : "clear", ep->endpoint.bEndpointAddress,
		       ep->thread_info.ep_num, rv < 0 ? strerror(-rv) : "ok");
		g.halted[slot][d] = halt;
	}
}

// Caller holds g.mtx; configured && bound_slot >= 0 && !running.
void start_locked(void)
{
	set_halt_locked(g.bound_slot, false);
	eps_start(g.fd, slot_alt(g.bound_slot));
	g.running = true;
	usleep(10 * 1000);	// give the threads time to spawn (as ep0_loop does)
	printf("[%.3f] bridge: %s slot forwarding\n", uptime_s(), slot_name(g.bound_slot));
}

// Caller holds g.mtx; running.
void stop_locked(bool drain)
{
	if (drain) {
		bool ok = drain_in_queues();
		if (verbose_level && !ok)
			printf("bridge: IN queue flush timed out\n");
	}
	eps_stop(slot_alt(g.bound_slot));
	g.running = false;
}

} // namespace

void bridge_setup_desc(void)
{
	host_device_desc.device = {};
	host_device_desc.device.bLength = USB_DT_DEVICE_SIZE;
	host_device_desc.device.bDescriptorType = USB_DT_DEVICE;
	host_device_desc.device.bNumConfigurations = 1;

	host_device_desc.configs = new struct raw_gadget_config[1];
	struct raw_gadget_config *cfg = &host_device_desc.configs[0];
	cfg->config = {};
	cfg->config.bLength = USB_DT_CONFIG_SIZE;
	cfg->config.bDescriptorType = USB_DT_CONFIG;
	cfg->config.bNumInterfaces = BRIDGE_NUM_SLOTS;
	cfg->config.bConfigurationValue = 1;
	cfg->config.bmAttributes = USB_CONFIG_ATT_ONE;
	cfg->config.bMaxPower = 250;
	cfg->interfaces = new struct raw_gadget_interface[BRIDGE_NUM_SLOTS];

	for (int s = 0; s < BRIDGE_NUM_SLOTS; s++) {
		struct raw_gadget_interface *iface = &cfg->interfaces[s];
		iface->num_altsettings = 1;
		iface->current_altsetting = 0;
		iface->altsettings = new struct raw_gadget_altsetting[1];
		struct raw_gadget_altsetting *alt = &iface->altsettings[0];
		alt->interface = {};
		alt->interface.bLength = USB_DT_INTERFACE_SIZE;
		alt->interface.bDescriptorType = USB_DT_INTERFACE;
		alt->interface.bInterfaceNumber = s;
		alt->interface.bAlternateSetting = 0;
		alt->interface.bNumEndpoints = 2;
		alt->interface.bInterfaceClass = 0xff;
		alt->interface.bInterfaceSubClass = 0x42;
		alt->interface.bInterfaceProtocol = s == BRIDGE_SLOT_ADB ? 0x01 : 0x03;
		alt->interface.iInterface = 0;
		alt->endpoints = new struct raw_gadget_endpoint[2];
		for (int k = 0; k < 2; k++) {
			struct raw_gadget_endpoint *ep = &alt->endpoints[k];
			memset((void *)ep, 0, sizeof(*ep));
			ep->endpoint.bLength = USB_DT_ENDPOINT_SIZE;
			ep->endpoint.bDescriptorType = USB_DT_ENDPOINT;
			// Placeholders; remap_host_endpoints_if_needed() assigns the
			// UDC's real endpoints.
			ep->endpoint.bEndpointAddress = (s + 1) | (k == 0 ? USB_DIR_IN : 0);
			ep->endpoint.bmAttributes = USB_ENDPOINT_XFER_BULK;
			ep->endpoint.wMaxPacketSize = 512;
			ep->endpoint.bInterval = 0;
			ep->device_bEndpointAddress = ep->endpoint.bEndpointAddress;
			ep->thread_info.ep_num = -1;
		}
	}
	host_device_desc.current_config = 0;
}

void bridge_set_interface_base(uint8_t base)
{
	for (int s = 0; s < BRIDGE_NUM_SLOTS; s++)
		slot_alt(s)->interface.bInterfaceNumber = base + s;
}

void bridge_init(int fd)
{
	std::lock_guard<std::mutex> guard(g.mtx);
	g.fd = fd;
	g.configured = false;
	g.bound_slot = -1;
	g.running = false;
	memset(g.halted, 0, sizeof(g.halted));
}

void bridge_host_configured(void)
{
	std::lock_guard<std::mutex> guard(g.mtx);
	if (g.configured)
		return;
	for (int s = 0; s < BRIDGE_NUM_SLOTS; s++) {
		eps_enable(g.fd, slot_alt(s));
		set_halt_locked(s, true);
	}
	g.configured = true;
	if (g.bound_slot >= 0)
		start_locked();
}

void bridge_host_unconfigured(void)
{
	std::lock_guard<std::mutex> guard(g.mtx);
	if (!g.configured)
		return;
	if (g.running)
		stop_locked(false);
	for (int s = 0; s < BRIDGE_NUM_SLOTS; s++)
		eps_disable(g.fd, slot_alt(s));
	memset(g.halted, 0, sizeof(g.halted));
	g.configured = false;
}

bool bridge_ep_is_ours(uint8_t addr)
{
	int s, d;
	return find_ep(addr, &s, &d);
}

bool bridge_ep_halted(uint8_t addr)
{
	int s, d;
	if (!find_ep(addr, &s, &d))
		return false;
	std::lock_guard<std::mutex> guard(g.mtx);
	return g.halted[s][d];
}

void bridge_ep_set_halt(uint8_t addr, bool halt)
{
	int s, d;
	if (!find_ep(addr, &s, &d))
		return;
	std::lock_guard<std::mutex> guard(g.mtx);
	struct raw_gadget_endpoint *ep = slot_ep(s, d == 0);
	if (!g.configured || !ep || ep->thread_info.ep_num < 0)
		return;
	int rv = halt ? usb_raw_ep_set_halt_try(g.fd, ep->thread_info.ep_num)
		      : usb_raw_ep_clear_halt_try(g.fd, ep->thread_info.ep_num);
	if (rv < 0 && verbose_level)
		printf("bridge: host %s halt on EP%02x: %d\n",
		       halt ? "set" : "clear", addr, rv);
	g.halted[s][d] = halt;
}

bool bridge_iface_is_ours(uint8_t iface)
{
	for (int s = 0; s < BRIDGE_NUM_SLOTS; s++)
		if (slot_alt(s)->interface.bInterfaceNumber == iface)
			return true;
	return false;
}

bool bridge_bind(int slot, uint8_t dev_in_addr, uint8_t dev_out_addr)
{
	std::lock_guard<std::mutex> guard(g.mtx);
	if (g.bound_slot >= 0) {
		printf("bridge: already bound to the %s slot\n", slot_name(g.bound_slot));
		return false;
	}
	struct raw_gadget_endpoint *in = slot_ep(slot, true);
	struct raw_gadget_endpoint *out = slot_ep(slot, false);
	if (!in || !out)
		return false;
	in->device_bEndpointAddress = dev_in_addr;
	out->device_bEndpointAddress = dev_out_addr;
	g.bound_slot = slot;
	if (slot == BRIDGE_SLOT_ADB) {
		std::lock_guard<std::mutex> l(g_out.mtx);
		// Whatever the host still owes of the previous message belonged
		// to the device that is gone: drop it, resume at the boundary.
		if (g_out.payload_remaining > 0) {
			printf("[%.3f] bridge: host mid-message (%u bytes to go) from the previous "
			       "device; dropping that tail\n", uptime_s(), g_out.payload_remaining);
			g_out.dropping = true;
		}
		g_out.cnxn_seen_since_bind = false;
		g_out.cnxn_building.clear();
		g_out.cnxn_building_left = 0;
	}
	printf("[%.3f] bridge: %s slot bound (gadget EP%02x/EP%02x <-> device EP%02x/EP%02x)%s\n",
	       uptime_s(), slot_name(slot),
	       in->endpoint.bEndpointAddress, out->endpoint.bEndpointAddress,
	       dev_in_addr, dev_out_addr,
	       g.configured ? "" : ", host not configured yet");
	if (g.configured)
		start_locked();
	return true;
}

void bridge_unbind(void)
{
	std::lock_guard<std::mutex> guard(g.mtx);
	if (g.bound_slot < 0)
		return;
	int slot = g.bound_slot;
	if (g.running)
		stop_locked(true);
	if (g.configured)
		set_halt_locked(slot, true);
	g.bound_slot = -1;
	printf("[%.3f] bridge: %s slot unbound%s\n", uptime_s(), slot_name(slot),
	       g.configured ? " (halted until the next device)" : "");
}

bool bridge_wait_device_lost(int timeout_ms)
{
	std::unique_lock<std::mutex> lock(g.lost_mtx);
	auto pred = [] { return g.lost || please_stop_ep0; };
	if (timeout_ms < 0) {
		// Bounded waits so please_stop_ep0 (set from a signal handler,
		// nobody notifies) is noticed.
		while (!pred())
			g.lost_cv.wait_for(lock, std::chrono::milliseconds(500));
	} else {
		g.lost_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), pred);
	}
	return g.lost;
}

bool bridge_device_lost_pending(void)
{
	std::lock_guard<std::mutex> l(g.lost_mtx);
	return g.lost;
}

void bridge_clear_device_lost(void)
{
	std::lock_guard<std::mutex> l(g.lost_mtx);
	g.lost = false;
}

void bridge_note_device_lost(const char *where)
{
	std::lock_guard<std::mutex> l(g.lost_mtx);
	if (!g.lost) {
		g.lost = true;
		printf("[%.3f] bridge: device lost (%s)\n", uptime_s(), where);
	}
	g.lost_cv.notify_all();
}

/*----------------------------------------------------------------------*/
/* Host->device adb stream framing                                       */

// adb writes each transport header (24 bytes) as its own USB transfer and
// the payload as another, so a gadget read never spans a message boundary
// (the ack accelerator in proxy.cpp relies on the same). One decision per
// read is therefore enough.
bool bridge_out_feed(const uint8_t *data, int len)
{
	g_out.mtx.lock();
	if (g.bound_slot != BRIDGE_SLOT_ADB || len <= 0)
		return true;

	if (g_out.payload_remaining > 0) {
		uint32_t take = (uint32_t)len < g_out.payload_remaining ? len : g_out.payload_remaining;
		g_out.payload_remaining -= take;
		if (g_out.cnxn_building_left > 0) {
			g_out.cnxn_building.insert(g_out.cnxn_building.end(), data, data + take);
			g_out.cnxn_building_left -= take;
			if (g_out.cnxn_building_left == 0 && !g_out.dropping) {
				g_out.cnxn = g_out.cnxn_building;
				g_out.cnxn_building.clear();
			}
		}
		bool forward = !g_out.dropping;
		if (g_out.payload_remaining == 0 && g_out.dropping) {
			g_out.dropping = false;
			printf("[%.3f] bridge: host stream back at a message boundary\n", uptime_s());
		}
		return forward;
	}

	// A header is due.
	uint32_t cmd, plen;
	if (len != 24 || !parse_hdr(data, &cmd, &plen)) {
		if (!g_out.unsynced)
			printf("[%.3f] bridge: %d host bytes where a header was due; dropping until "
			       "one arrives\n", uptime_s(), len);
		g_out.unsynced = true;
		return false;
	}
	if (g_out.unsynced) {
		printf("[%.3f] bridge: host stream re-synced\n", uptime_s());
		g_out.unsynced = false;
	}
	g_out.payload_remaining = plen;
	if (cmd == A_CNXN) {
		g_out.cnxn_seen_since_bind = true;
		g_out.cnxn_building.assign(data, data + 24);
		g_out.cnxn_building_left = plen;
		if (plen == 0)
			g_out.cnxn = g_out.cnxn_building;
	}
	return true;
}

void bridge_out_done(void)
{
	g_out.mtx.unlock();
}

void bridge_replay_cnxn_if_needed(void)
{
	std::lock_guard<std::mutex> l(g_out.mtx);
	if (g.bound_slot != BRIDGE_SLOT_ADB || g_out.cnxn_seen_since_bind)
		return;
	if (g_out.cnxn.size() < 24) {
		printf("[%.3f] bridge: host sent no CNXN since the bind and none is on record; "
		       "adb will connect when the host re-opens its transport\n", uptime_s());
		return;
	}
	if (g_out.payload_remaining > 0 || g_out.unsynced)
		return;		// not at a boundary; the host is still talking

	struct raw_gadget_endpoint *out = slot_ep(BRIDGE_SLOT_ADB, false);
	uint8_t dev_ep = out->device_bEndpointAddress;
	size_t plen = g_out.cnxn.size() - 24;
	printf("[%.3f] bridge: host sent no CNXN since the bind; replaying its last one "
	       "(%zu-byte banner) to the device\n", uptime_s(), plen);
	// Header and payload as two transfers, as adb sends them.
	for (int part = 0; part < 2; part++) {
		size_t off = part == 0 ? 0 : 24;
		size_t n = part == 0 ? 24 : plen;
		if (n == 0)
			continue;
		uint8_t *buf = new uint8_t[n];
		memcpy(buf, g_out.cnxn.data() + off, n);
		int rv;
		if (bulk_out_max_in_flight > 0) {
			rv = send_data_async(dev_ep, buf, n, USB_REQUEST_TIMEOUT);
		} else {
			rv = send_data(dev_ep, USB_ENDPOINT_XFER_BULK, buf, n, USB_REQUEST_TIMEOUT);
			delete[] buf;
		}
		if (rv != LIBUSB_SUCCESS) {
			printf("[%.3f] bridge: CNXN replay failed: %d\n", uptime_s(), rv);
			if (rv == LIBUSB_ERROR_NO_DEVICE)
				bridge_note_device_lost("CNXN replay");
			return;
		}
	}
	g_out.cnxn_seen_since_bind = true;
}

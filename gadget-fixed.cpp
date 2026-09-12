#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <pthread.h>

#include "bridge.h"
#include "console-acm.h"
#include "console-shell.h"
#include "gadget-fixed.h"
#include "host-raw-gadget.h"
#include "misc.h"

namespace {

struct FixedGadget {
	int fd = -1;
	pthread_t th = 0;
	std::atomic<bool> stop{false};
	bool attached = false;
	uint8_t configured = 0;
};

FixedGadget g_fixed;

/*----------------------------------------------------------------------*/
/* Descriptors                                                          */

struct Writer {
	uint8_t *p;
	size_t n;
	size_t cap;
	void u8(uint8_t v) { if (n < cap) p[n] = v; n++; }
	void u16(uint16_t v) { u8(v & 0xff); u8(v >> 8); }
};

size_t build_device(uint8_t *out, size_t cap)
{
	if (cap < USB_DT_DEVICE_SIZE)
		return 0;
	Writer w = {out, 0, cap};
	w.u8(USB_DT_DEVICE_SIZE); w.u8(USB_DT_DEVICE);
	w.u16(0x0200);				// bcdUSB 2.00
	w.u8(USB_CLASS_MISC); w.u8(0x02); w.u8(0x01);	// IAD
	w.u8(64);				// bMaxPacketSize0
	w.u16(gadget_vendor_id);
	w.u16(gadget_product_id);
	w.u16(0x0100);				// bcdDevice
	w.u8(1); w.u8(2); w.u8(3);		// iManufacturer, iProduct, iSerialNumber
	w.u8(1);				// bNumConfigurations
	return w.n;
}

size_t build_qualifier(uint8_t *out, size_t cap)
{
	if (cap < 10)
		return 0;
	Writer w = {out, 0, cap};
	w.u8(10); w.u8(USB_DT_DEVICE_QUALIFIER);
	w.u16(0x0200);
	w.u8(USB_CLASS_MISC); w.u8(0x02); w.u8(0x01);
	w.u8(64); w.u8(1); w.u8(0);
	return w.n;
}

// CONFIG / OTHER_SPEED_CONFIG: header, the console's IAD+CDC block (interfaces
// 0/1) when reserved, then the bridge interfaces from host_device_desc with
// the UDC endpoint addresses the remapper assigned.
size_t build_config(uint8_t type, uint8_t *out, size_t cap)
{
	bool high_speed = type == USB_DT_CONFIG;
	struct raw_gadget_config *cfg = &host_device_desc.configs[0];
	Writer w = {out, 0, cap};

	w.u8(USB_DT_CONFIG_SIZE); w.u8(type);
	w.u16(0);				// wTotalLength, patched below
	w.u8(cfg->config.bNumInterfaces + (g_acm.reserved ? ACM_NUM_INTERFACES : 0));
	w.u8(1);				// bConfigurationValue
	w.u8(0);				// iConfiguration
	w.u8(USB_CONFIG_ATT_ONE);		// bus powered
	w.u8(250);				// 500 mA

	if (g_acm.reserved) {
		if (w.n + ACM_DESC_LEN > cap)
			return 0;
		size_t n = acm_build_descriptors(g_acm.comm_if, high_speed, out + w.n, cap - w.n);
		if (n == 0)
			return 0;
		w.n += n;
	}

	for (int i = 0; i < cfg->config.bNumInterfaces; i++) {
		struct raw_gadget_altsetting *alt = &cfg->interfaces[i].altsettings[0];
		const struct usb_interface_descriptor &d = alt->interface;
		w.u8(USB_DT_INTERFACE_SIZE); w.u8(USB_DT_INTERFACE);
		w.u8(d.bInterfaceNumber); w.u8(0); w.u8(d.bNumEndpoints);
		w.u8(d.bInterfaceClass); w.u8(d.bInterfaceSubClass); w.u8(d.bInterfaceProtocol);
		w.u8(0);
		for (int k = 0; k < d.bNumEndpoints; k++) {
			const struct usb_endpoint_descriptor &e = alt->endpoints[k].endpoint;
			w.u8(USB_DT_ENDPOINT_SIZE); w.u8(USB_DT_ENDPOINT);
			w.u8(e.bEndpointAddress); w.u8(e.bmAttributes);
			w.u16(high_speed ? 512 : 64); w.u8(0);
		}
	}
	if (w.n > cap)
		return 0;
	out[2] = w.n & 0xff;
	out[3] = w.n >> 8;
	return w.n;
}

size_t build_string(uint8_t index, uint8_t *out, size_t cap)
{
	if (index == 0) {
		if (cap < 4)
			return 0;
		out[0] = 4; out[1] = USB_DT_STRING; out[2] = 0x09; out[3] = 0x04;
		return 4;
	}
	const char *s;
	switch (index) {
	case 1: s = "usb-proxy"; break;
	case 2: s = "usb-proxy"; break;
	case 3: s = gadget_serial.c_str(); break;
	default: return 0;
	}
	size_t len = strlen(s);
	size_t total = 2 + 2 * len;
	if (total > 255 || cap < total)
		return 0;
	out[0] = total;
	out[1] = USB_DT_STRING;
	for (size_t i = 0; i < len; i++) {
		out[2 + 2 * i] = s[i];
		out[3 + 2 * i] = 0;
	}
	return total;
}

/*----------------------------------------------------------------------*/
/* ep0                                                                  */

void reply(int fd, const uint8_t *data, size_t n, uint16_t wlength)
{
	struct usb_raw_transfer_io io;
	io.inner.ep = 0;
	io.inner.flags = 0;
	if (n > wlength)
		n = wlength;
	memcpy(io.data, data, n);
	io.inner.length = n;
	int rv = usb_raw_ep0_write_try(fd, (struct usb_raw_ep_io *)&io);
	if (rv < 0 && verbose_level)
		printf("fixed gadget: ep0 write failed: %d\n", rv);
}

void ack(int fd)
{
	struct usb_raw_ep_io io = {0, 0, 0};
	int rv = usb_raw_ep0_read_try(fd, &io);
	if (rv < 0 && verbose_level)
		printf("fixed gadget: ep0 ack failed: %d\n", rv);
}

void stall(int fd)
{
	usb_raw_ep0_stall_try(fd);
}

void unconfigure(int fd)
{
	bridge_host_unconfigured();
	acm_stop(fd);
	g_fixed.configured = 0;
}

void handle_control(int fd, struct usb_ctrlrequest *ctrl)
{
	if (verbose_level > 1)
		log_control_request(ctrl);

	// The console's interfaces/endpoints and its class requests.
	if (acm_handle_ep0(fd, ctrl))
		return;

	uint8_t recip = ctrl->bRequestType & USB_RECIP_MASK;
	if ((ctrl->bRequestType & USB_TYPE_MASK) != USB_TYPE_STANDARD) {
		stall(fd);
		return;
	}
	static const uint8_t zero2[2] = {0, 0};

	if (recip == USB_RECIP_INTERFACE) {
		if (!bridge_iface_is_ours(ctrl->wIndex & 0xff)) {
			stall(fd);
			return;
		}
		switch (ctrl->bRequest) {
		case USB_REQ_GET_STATUS:
			reply(fd, zero2, 2, ctrl->wLength);
			return;
		case USB_REQ_GET_INTERFACE:
			reply(fd, zero2, 1, ctrl->wLength);
			return;
		case USB_REQ_SET_INTERFACE:
			if ((ctrl->wValue & 0xff) == 0)
				ack(fd);
			else
				stall(fd);
			return;
		default:
			stall(fd);
			return;
		}
	}

	if (recip == USB_RECIP_ENDPOINT) {
		uint8_t addr = ctrl->wIndex & 0xff;
		if (!bridge_ep_is_ours(addr)) {
			stall(fd);
			return;
		}
		switch (ctrl->bRequest) {
		case USB_REQ_GET_STATUS: {
			uint8_t st[2] = {(uint8_t)(bridge_ep_halted(addr) ? 1 : 0), 0};
			reply(fd, st, 2, ctrl->wLength);
			return;
		}
		case USB_REQ_CLEAR_FEATURE:
			if (ctrl->wValue == USB_ENDPOINT_HALT) {
				printf("[%.3f] fixed gadget: host cleared halt on EP%02x\n",
				       uptime_s(), addr);
				bridge_ep_set_halt(addr, false);
			}
			ack(fd);
			return;
		case USB_REQ_SET_FEATURE:
			if (ctrl->wValue == USB_ENDPOINT_HALT)
				bridge_ep_set_halt(addr, true);
			ack(fd);
			return;
		default:
			stall(fd);
			return;
		}
	}

	if (recip != USB_RECIP_DEVICE) {
		stall(fd);
		return;
	}

	uint8_t buf[MAX_TRANSFER_SIZE];
	switch (ctrl->bRequest) {
	case USB_REQ_GET_DESCRIPTOR: {
		uint8_t type = ctrl->wValue >> 8;
		uint8_t index = ctrl->wValue & 0xff;
		size_t n = 0;
		switch (type) {
		case USB_DT_DEVICE:
			n = build_device(buf, sizeof(buf));
			break;
		case USB_DT_DEVICE_QUALIFIER:
			n = build_qualifier(buf, sizeof(buf));
			break;
		case USB_DT_CONFIG:
		case USB_DT_OTHER_SPEED_CONFIG:
			if (index == 0)
				n = build_config(type, buf, sizeof(buf));
			break;
		case USB_DT_STRING:
			n = build_string(index, buf, sizeof(buf));
			break;
		default:
			break;
		}
		if (n == 0) {
			stall(fd);
			return;
		}
		reply(fd, buf, n, ctrl->wLength);
		return;
	}
	case USB_REQ_SET_CONFIGURATION: {
		uint8_t value = ctrl->wValue & 0xff;
		if (value != 0 && value != 1) {
			stall(fd);
			return;
		}
		if (g_fixed.configured)
			unconfigure(fd);
		if (value == 1) {
			int rv = usb_raw_configure_try(fd);
			if (rv < 0 && verbose_level)
				printf("fixed gadget: configure: %d\n", rv);
			bridge_host_configured();
			acm_start(fd);
			printf("[%.3f] fixed gadget: host SET_CONFIGURATION\n", uptime_s());
		}
		g_fixed.configured = value;
		ack(fd);
		return;
	}
	case USB_REQ_GET_CONFIGURATION:
		reply(fd, &g_fixed.configured, 1, ctrl->wLength);
		return;
	case USB_REQ_GET_STATUS:
		reply(fd, zero2, 2, ctrl->wLength);
		return;
	case USB_REQ_SET_FEATURE:
	case USB_REQ_CLEAR_FEATURE:
		ack(fd);
		return;
	default:
		stall(fd);
		return;
	}
}

void *ep0_thread(void *)
{
	int fd = g_fixed.fd;

	// Like ep0_loop (proxy.cpp): the event fetch sleeps interruptibly, so
	// keep incidental signals (the power hook's SIGCHLD, ...) off this
	// thread. SIGUSR1 stays deliverable: it is how fixed_gadget_stop() gets
	// us out of the fetch, and an EINTR is simply retried. SIGINT/SIGTERM
	// likewise (the handler sets the stop flags; we retry).
	sigset_t block_set;
	sigfillset(&block_set);
	sigdelset(&block_set, SIGUSR1);
	sigdelset(&block_set, SIGINT);
	sigdelset(&block_set, SIGTERM);
	sigdelset(&block_set, SIGSEGV);
	sigdelset(&block_set, SIGBUS);
	sigdelset(&block_set, SIGFPE);
	sigdelset(&block_set, SIGILL);
	sigdelset(&block_set, SIGABRT);
	sigdelset(&block_set, SIGTRAP);
	sigdelset(&block_set, SIGSYS);
	pthread_sigmask(SIG_SETMASK, &block_set, nullptr);

	while (!g_fixed.stop) {
		struct usb_raw_control_event event;
		event.inner.type = 0;
		event.inner.length = sizeof(event.ctrl);
		int rv = usb_raw_event_fetch_try(fd, (struct usb_raw_event *)&event);
		if (rv < 0) {
			if (rv == -EINTR)
				continue;
			// The gadget is dead (raw-gadget STATE_DEV_FAILED or the
			// like). There is no "next attach" to recover on: exit, let
			// the service manager respawn a fresh instance. The one
			// re-enumeration this mode still has.
			printf("[%.3f] fixed gadget: event fetch failed: %d; exiting for a respawn\n",
			       uptime_s(), rv);
			shell_kill();
			fflush(stdout);
			_exit(1);
		}
		switch (event.inner.type) {
		case USB_RAW_EVENT_CONNECT:
			printf("[%.3f] fixed gadget: host connected\n", uptime_s());
			break;
		case USB_RAW_EVENT_RESET:
		case USB_RAW_EVENT_DISCONNECT:
			if (verbose_level)
				printf("[%.3f] fixed gadget: %s\n", uptime_s(),
				       event.inner.type == USB_RAW_EVENT_RESET ? "reset" : "disconnect");
			unconfigure(fd);
			acm_class_reset();
			break;
		case USB_RAW_EVENT_CONTROL:
			handle_control(fd, &event.ctrl);
			break;
		default:
			break;
		}
	}
	return nullptr;
}

} // namespace

bool fixed_gadget_start(const char *driver, const char *device)
{
	if (g_fixed.attached)
		return true;

	int fd = usb_raw_open_try();
	if (fd < 0) {
		printf("fixed gadget: open /dev/raw-gadget: %d\n", fd);
		return false;
	}
	int rv = usb_raw_init_try(fd, USB_SPEED_HIGH, driver, device);
	if (rv < 0) {
		printf("fixed gadget: init: %d\n", rv);
		close(fd);
		return false;
	}
	// The UDC may still be unbinding from a previous instance's gadget.
	rv = -EBUSY;
	for (int attempt = 0; attempt < 40 && rv == -EBUSY; attempt++) {
		rv = usb_raw_run_try(fd);
		if (rv == -EBUSY)
			usleep(50 * 1000);
	}
	if (rv < 0) {
		printf("fixed gadget: run: %d\n", rv);
		close(fd);
		return false;
	}

	if (remap_host_endpoints_if_needed(fd) < 0) {
		printf("fixed gadget: the UDC cannot host the adb + fastboot bridge\n");
		acm_unreserve();
		close(fd);
		return false;
	}
	if (g_acm.reserved) {
		g_acm.comm_if = 0;
		g_acm.data_if = 1;
		bridge_set_interface_base(ACM_NUM_INTERFACES);
	} else {
		bridge_set_interface_base(0);
	}

	bridge_init(fd);
	g_fixed.fd = fd;
	g_fixed.stop = false;
	g_fixed.configured = 0;
	if (pthread_create(&g_fixed.th, nullptr, ep0_thread, nullptr) != 0) {
		perror("fixed gadget: pthread_create");
		acm_unreserve();
		close(fd);
		g_fixed.fd = -1;
		return false;
	}
	g_fixed.attached = true;
	printf("[%.3f] fixed gadget: attached (%04x:%04x serial %s%s)\n", uptime_s(),
	       gadget_vendor_id, gadget_product_id, gadget_serial.c_str(),
	       g_acm.reserved ? ", console on interfaces 0/1" : ", no console");
	return true;
}

void fixed_gadget_stop(void)
{
	if (!g_fixed.attached)
		return;
	g_fixed.stop = true;
	pthread_kill(g_fixed.th, SIGUSR1);
	pthread_join(g_fixed.th, nullptr);
	g_fixed.th = 0;
	unconfigure(g_fixed.fd);
	acm_class_reset();
	acm_unreserve();
	close(g_fixed.fd);
	g_fixed.fd = -1;
	g_fixed.attached = false;
	printf("[%.3f] fixed gadget: detached\n", uptime_s());
}

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <pthread.h>
#include <vector>

#include "console-acm.h"
#include "gadget-idle.h"
#include "host-raw-gadget.h"
#include "misc.h"

namespace {

struct IdleGadget {
	int fd = -1;
	pthread_t th = 0;
	std::atomic<bool> stop{false};
	bool attached = false;
	bool ever_detached = false;
	std::chrono::steady_clock::time_point detached_at;
};

IdleGadget g_idle;

// Linux Foundation "Multifunction Composite Gadget", the id configfs gadgets
// use; nothing on a host claims it by VID/PID, the ACM interfaces are matched
// by class.
const uint8_t k_device_desc[18] = {
	18, USB_DT_DEVICE,
	0x00, 0x02,			// bcdUSB 2.00
	USB_CLASS_MISC, 0x02, 0x01,	// IAD
	64,				// bMaxPacketSize0
	0x6b, 0x1d,			// idVendor 1d6b
	0x04, 0x01,			// idProduct 0104
	0x00, 0x01,			// bcdDevice 1.00
	1, 2, 3,			// iManufacturer, iProduct, iSerialNumber
	1,				// bNumConfigurations
};

const uint8_t k_qualifier_desc[10] = {
	10, USB_DT_DEVICE_QUALIFIER,
	0x00, 0x02,
	USB_CLASS_MISC, 0x02, 0x01,
	64, 1, 0,
};

// Fixed strings so macOS derives a stable /dev/cu.usbmodem<serial>N name.
const char *k_strings[] = {"usb-proxy", "usb-proxy console", "USBPROXY01"};

size_t build_config(uint8_t type, uint8_t *out, size_t cap)
{
	if (cap < USB_DT_CONFIG_SIZE + ACM_DESC_LEN)
		return 0;
	uint16_t total = USB_DT_CONFIG_SIZE + ACM_DESC_LEN;
	out[0] = USB_DT_CONFIG_SIZE;
	out[1] = type;
	out[2] = total & 0xff;
	out[3] = total >> 8;
	out[4] = ACM_NUM_INTERFACES;
	out[5] = 1;			// bConfigurationValue
	out[6] = 0;			// iConfiguration
	out[7] = USB_CONFIG_ATT_ONE;	// bus powered
	out[8] = 250;			// 500 mA
	size_t n = acm_build_descriptors(0, type == USB_DT_CONFIG,
					 out + USB_DT_CONFIG_SIZE, cap - USB_DT_CONFIG_SIZE);
	return n ? USB_DT_CONFIG_SIZE + n : 0;
}

size_t build_string(uint8_t index, uint8_t *out, size_t cap)
{
	if (index == 0) {
		if (cap < 4)
			return 0;
		out[0] = 4; out[1] = USB_DT_STRING; out[2] = 0x09; out[3] = 0x04;
		return 4;
	}
	if (index > 3)
		return 0;
	const char *s = k_strings[index - 1];
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
		printf("idle gadget: ep0 write failed: %d\n", rv);
}

void ack(int fd)
{
	struct usb_raw_ep_io io = {0, 0, 0};
	int rv = usb_raw_ep0_read_try(fd, &io);
	if (rv < 0 && verbose_level)
		printf("idle gadget: ep0 ack failed: %d\n", rv);
}

void stall(int fd)
{
	usb_raw_ep0_stall_try(fd);
}

uint8_t g_configured = 0;

void handle_control(int fd, struct usb_ctrlrequest *ctrl)
{
	if (verbose_level > 1)
		log_control_request(ctrl);

	// Class requests and anything aimed at the ACM interfaces/endpoints.
	if (acm_handle_ep0(fd, ctrl))
		return;

	uint8_t recip = ctrl->bRequestType & USB_RECIP_MASK;
	if ((ctrl->bRequestType & USB_TYPE_MASK) != USB_TYPE_STANDARD ||
	    recip != USB_RECIP_DEVICE) {
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
			memcpy(buf, k_device_desc, sizeof(k_device_desc));
			n = sizeof(k_device_desc);
			break;
		case USB_DT_DEVICE_QUALIFIER:
			memcpy(buf, k_qualifier_desc, sizeof(k_qualifier_desc));
			n = sizeof(k_qualifier_desc);
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
		acm_stop(fd);
		if (value == 1) {
			int rv = usb_raw_configure_try(fd);
			if (rv < 0 && verbose_level)
				printf("idle gadget: configure: %d\n", rv);
			acm_start(fd);
			printf("[%.3f] idle gadget: host SET_CONFIGURATION\n", uptime_s());
		}
		g_configured = value;
		ack(fd);
		return;
	}
	case USB_REQ_GET_CONFIGURATION:
		reply(fd, &g_configured, 1, ctrl->wLength);
		return;
	case USB_REQ_GET_STATUS: {
		uint8_t st[2] = {0, 0};
		reply(fd, st, 2, ctrl->wLength);
		return;
	}
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
	int fd = g_idle.fd;
	while (!g_idle.stop) {
		struct usb_raw_control_event event;
		event.inner.type = 0;
		event.inner.length = sizeof(event.ctrl);
		int rv = usb_raw_event_fetch_try(fd, (struct usb_raw_event *)&event);
		if (rv < 0) {
			if (rv == -EINTR)
				continue;
			printf("idle gadget: event fetch failed: %d; console off until "
			       "the next attach\n", rv);
			break;
		}
		switch (event.inner.type) {
		case USB_RAW_EVENT_CONNECT:
			printf("[%.3f] idle gadget: host connected\n", uptime_s());
			break;
		case USB_RAW_EVENT_RESET:
		case USB_RAW_EVENT_DISCONNECT:
			acm_stop(fd);
			acm_class_reset();
			g_configured = 0;
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

bool idle_gadget_start(const char *driver, const char *device)
{
	if (g_idle.attached)
		return true;

	int fd = usb_raw_open_try();
	if (fd < 0) {
		printf("idle gadget: open /dev/raw-gadget: %d\n", fd);
		return false;
	}
	int rv = usb_raw_init_try(fd, USB_SPEED_HIGH, driver, device);
	if (rv < 0) {
		printf("idle gadget: init: %d\n", rv);
		close(fd);
		return false;
	}
	rv = usb_raw_run_try(fd);
	if (rv < 0) {
		printf("idle gadget: run: %d\n", rv);
		close(fd);
		return false;
	}

	struct usb_raw_eps_info info;
	memset(&info, 0, sizeof(info));
	int num = usb_raw_eps_info_try(fd, &info);
	std::vector<bool> reserved;
	if (num <= 0 || !acm_reserve_endpoints(info, num, reserved)) {
		printf("idle gadget: no endpoints for the console (%d); detaching\n", num);
		close(fd);
		return false;
	}

	g_acm.comm_if = 0;
	g_acm.data_if = 1;
	g_idle.fd = fd;
	g_idle.stop = false;
	g_configured = 0;
	if (pthread_create(&g_idle.th, nullptr, ep0_thread, nullptr) != 0) {
		perror("idle gadget: pthread_create");
		acm_unreserve();
		close(fd);
		g_idle.fd = -1;
		return false;
	}
	g_idle.attached = true;
	printf("[%.3f] idle gadget: console-only gadget attached\n", uptime_s());
	return true;
}

void idle_gadget_stop(void)
{
	if (!g_idle.attached)
		return;
	g_idle.stop = true;
	pthread_kill(g_idle.th, SIGUSR1);
	pthread_join(g_idle.th, nullptr);
	g_idle.th = 0;
	acm_stop(g_idle.fd);
	acm_class_reset();
	acm_unreserve();
	// Releasing the fd unregisters the gadget: D+ drops, the host sees a
	// disconnect.
	close(g_idle.fd);
	g_idle.fd = -1;
	g_idle.attached = false;
	g_idle.ever_detached = true;
	g_idle.detached_at = std::chrono::steady_clock::now();
	printf("[%.3f] idle gadget: detached\n", uptime_s());
}

bool idle_gadget_attached(void)
{
	return g_idle.attached;
}

void idle_gadget_wait_min_off(int min_off_ms)
{
	if (!g_idle.ever_detached || min_off_ms <= 0)
		return;
	auto since = std::chrono::steady_clock::now() - g_idle.detached_at;
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(since).count();
	if (ms < min_off_ms) {
		printf("[%.3f] holding the gadget off for %lld ms more (min_off_ms %d)\n",
		       uptime_s(), (long long)(min_off_ms - ms), min_off_ms);
		usleep((min_off_ms - ms) * 1000);
	}
}

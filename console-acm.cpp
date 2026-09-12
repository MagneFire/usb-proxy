#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <linux/usb/cdc.h>

#include "console-acm.h"
#include "console-shell.h"
#include "misc.h"

AcmFunction g_acm;

/*----------------------------------------------------------------------*/
/* Descriptors                                                          */

namespace {

struct DescWriter {
	uint8_t *p;
	size_t n;
	size_t cap;
	void u8(uint8_t v) { if (n < cap) p[n] = v; n++; }
	void u16(uint16_t v) { u8(v & 0xff); u8(v >> 8); }
};

void unblock_sigusr1(void)
{
	sigset_t s;
	sigemptyset(&s);
	sigaddset(&s, SIGUSR1);
	pthread_sigmask(SIG_UNBLOCK, &s, nullptr);
}

} // namespace

size_t acm_build_descriptors(uint8_t comm_if, bool high_speed,
			     uint8_t *out, size_t cap)
{
	if (!g_acm.reserved || cap < ACM_DESC_LEN)
		return 0;
	uint8_t data_if = comm_if + 1;
	uint16_t bulk_maxp = high_speed ? 512 : 64;
	// Notification interval: 32 ms either way (2^(9-1) HS microframes, 32 FS
	// frames), the value Linux's f_acm uses.
	uint8_t notify_interval = high_speed ? 9 : 32;
	DescWriter w = {out, 0, cap};

	// Interface Association: the two interfaces form one CDC-ACM function.
	w.u8(8); w.u8(USB_DT_INTERFACE_ASSOCIATION);
	w.u8(comm_if); w.u8(ACM_NUM_INTERFACES);
	w.u8(USB_CLASS_COMM); w.u8(USB_CDC_SUBCLASS_ACM); w.u8(USB_CDC_ACM_PROTO_AT_V25TER);
	w.u8(0);
	// Communication class interface.
	w.u8(9); w.u8(USB_DT_INTERFACE);
	w.u8(comm_if); w.u8(0); w.u8(1);
	w.u8(USB_CLASS_COMM); w.u8(USB_CDC_SUBCLASS_ACM); w.u8(USB_CDC_ACM_PROTO_AT_V25TER);
	w.u8(0);
	// CDC Header functional descriptor, bcdCDC 1.10.
	w.u8(5); w.u8(USB_DT_CS_INTERFACE); w.u8(USB_CDC_HEADER_TYPE); w.u16(0x0110);
	// Call Management: no call management, data interface number.
	w.u8(5); w.u8(USB_DT_CS_INTERFACE); w.u8(USB_CDC_CALL_MANAGEMENT_TYPE);
	w.u8(0); w.u8(data_if);
	// ACM functional descriptor. bmCapabilities = 0: we do NOT advertise
	// Set/Get_Line_Coding or Set_Control_Line_State. A console has no real
	// baud rate, and more to the point SET_LINE_CODING is a control-OUT WITH
	// a data stage, which on this musb UDC through raw-gadget is racy: two
	// back-to-back ones (macOS sends a pair when an app opens the port) make
	// the second EP0 OUT queue in the wrong ep0 stage -> usb_ep_queue -EINVAL
	// -> the raw-gadget device goes STATE_DEV_FAILED and the whole proxy dies
	// and respawns (re-enumerating the proxied device too). With D1 clear a
	// host does not send them; output then flows whenever the host reads,
	// like a UART with hardware flow control.
	w.u8(4); w.u8(USB_DT_CS_INTERFACE); w.u8(USB_CDC_ACM_TYPE);
	w.u8(0x00);
	// Union: master = comm, slave = data.
	w.u8(5); w.u8(USB_DT_CS_INTERFACE); w.u8(USB_CDC_UNION_TYPE);
	w.u8(comm_if); w.u8(data_if);
	// Notification endpoint (interrupt IN).
	w.u8(7); w.u8(USB_DT_ENDPOINT);
	w.u8(g_acm.notify_ep.bEndpointAddress); w.u8(USB_ENDPOINT_XFER_INT);
	w.u16(10); w.u8(notify_interval);
	// Data class interface with the two bulk endpoints.
	w.u8(9); w.u8(USB_DT_INTERFACE);
	w.u8(data_if); w.u8(0); w.u8(2);
	w.u8(USB_CLASS_CDC_DATA); w.u8(0); w.u8(0);
	w.u8(0);
	w.u8(7); w.u8(USB_DT_ENDPOINT);
	w.u8(g_acm.in_ep.bEndpointAddress); w.u8(USB_ENDPOINT_XFER_BULK);
	w.u16(bulk_maxp); w.u8(0);
	w.u8(7); w.u8(USB_DT_ENDPOINT);
	w.u8(g_acm.out_ep.bEndpointAddress); w.u8(USB_ENDPOINT_XFER_BULK);
	w.u16(bulk_maxp); w.u8(0);

	if (w.n != ACM_DESC_LEN) {
		fprintf(stderr, "console: descriptor block is %zu bytes, expected %d\n",
			w.n, ACM_DESC_LEN);
		return 0;
	}
	return w.n;
}

/*----------------------------------------------------------------------*/
/* Endpoint reservation                                                 */

namespace {

// Last unreserved candidate (walking from the tail) with the wanted caps.
int pick_from_tail(const struct usb_raw_eps_info &info, int num,
		   const std::vector<bool> &reserved, bool dir_in, bool want_int)
{
	for (int i = num - 1; i >= 0; i--) {
		if (reserved[i])
			continue;
		const struct usb_raw_ep_caps &c = info.eps[i].caps;
		if (dir_in && !c.dir_in)
			continue;
		if (!dir_in && !c.dir_out)
			continue;
		if (want_int ? !c.type_int : !c.type_bulk)
			continue;
		return i;
	}
	return -1;
}

void fill_ep(struct usb_endpoint_descriptor &d, const struct usb_raw_ep_info &ep,
	     bool dir_in, uint8_t type, uint16_t maxp, uint8_t interval,
	     uint8_t any_addr)
{
	memset(&d, 0, sizeof(d));
	d.bLength = USB_DT_ENDPOINT_SIZE;
	d.bDescriptorType = USB_DT_ENDPOINT;
	// raw-gadget's EP_ENABLE matches the descriptor's endpoint number against
	// the UDC endpoint's fixed address (musb: ep1..ep5), so it must carry the
	// real number. ADDR_ANY controllers (dummy_udc) take whatever we choose.
	uint8_t addr = ep.addr == USB_RAW_EP_ADDR_ANY ? any_addr : (uint8_t)ep.addr;
	d.bEndpointAddress = (addr & 0x0f) | (dir_in ? USB_DIR_IN : 0);
	d.bmAttributes = type;
	if (ep.limits.maxpacket_limit && maxp > ep.limits.maxpacket_limit)
		maxp = ep.limits.maxpacket_limit;
	d.wMaxPacketSize = maxp;
	d.bInterval = interval;
}

} // namespace

bool acm_reserve_endpoints(const struct usb_raw_eps_info &info, int num,
			   std::vector<bool> &reserved)
{
	g_acm.reserved = false;
	if (num <= 0)
		return false;
	if ((int)reserved.size() < num)
		reserved.resize(num, false);

	int out_i = pick_from_tail(info, num, reserved, false, false);
	if (out_i >= 0)
		reserved[out_i] = true;
	int in_i = pick_from_tail(info, num, reserved, true, false);
	if (in_i >= 0)
		reserved[in_i] = true;
	int nt_i = pick_from_tail(info, num, reserved, true, true);
	if (nt_i >= 0)
		reserved[nt_i] = true;

	if (out_i < 0 || in_i < 0 || nt_i < 0) {
		if (out_i >= 0) reserved[out_i] = false;
		if (in_i >= 0) reserved[in_i] = false;
		if (nt_i >= 0) reserved[nt_i] = false;
		printf("console: no free UDC endpoints for CDC-ACM (need INT IN, "
		       "BULK IN, BULK OUT); console disabled for this device\n");
		return false;
	}

	fill_ep(g_acm.out_ep, info.eps[out_i], false, USB_ENDPOINT_XFER_BULK, 512, 0, 0x0f);
	fill_ep(g_acm.in_ep, info.eps[in_i], true, USB_ENDPOINT_XFER_BULK, 512, 0, 0x0f);
	fill_ep(g_acm.notify_ep, info.eps[nt_i], true, USB_ENDPOINT_XFER_INT, 10, 9, 0x0e);
	g_acm.reserved = true;
	printf("console: CDC-ACM on UDC endpoints %s (0x%02x), %s (0x%02x), %s (0x%02x)\n",
	       info.eps[nt_i].name, g_acm.notify_ep.bEndpointAddress,
	       info.eps[in_i].name, g_acm.in_ep.bEndpointAddress,
	       info.eps[out_i].name, g_acm.out_ep.bEndpointAddress);
	return true;
}

void acm_unreserve(void)
{
	g_acm.reserved = false;
	g_acm.comm_if = g_acm.data_if = 0xff;
}

/*----------------------------------------------------------------------*/
/* Descriptor splice (composite mode)                                   */

void acm_rewrite_descriptor(const struct usb_ctrlrequest *ctrl,
			    struct usb_raw_transfer_io &io, uint16_t wlength)
{
	if (!g_acm.reserved)
		return;
	if ((ctrl->bRequestType & USB_TYPE_MASK) != USB_TYPE_STANDARD ||
	    (ctrl->bRequestType & USB_RECIP_MASK) != USB_RECIP_DEVICE ||
	    ctrl->bRequest != USB_REQ_GET_DESCRIPTOR)
		return;

	uint8_t type = ctrl->wValue >> 8;
	uint8_t *d = (uint8_t *)io.data;
	size_t len = io.inner.length;

	if (type == USB_DT_DEVICE) {
		// A class-00 device relies on per-interface matching; with an IAD
		// in the configuration the spec wants Miscellaneous/Common/IAD.
		if (len >= 8 && d[4] == 0 && d[5] == 0 && d[6] == 0) {
			d[4] = USB_CLASS_MISC;
			d[5] = 0x02;
			d[6] = 0x01;
		}
		return;
	}

	if (type != USB_DT_CONFIG && type != USB_DT_OTHER_SPEED_CONFIG)
		return;
	if (len < USB_DT_CONFIG_SIZE)
		return;

	uint16_t total = d[2] | (d[3] << 8);
	uint8_t nif = d[4];
	uint16_t new_total = total + ACM_DESC_LEN;
	d[2] = new_total & 0xff;
	d[3] = new_total >> 8;
	d[4] = nif + ACM_NUM_INTERFACES;
	g_acm.comm_if = nif;
	g_acm.data_if = nif + 1;

	// The device answered with its complete descriptor and the host asked
	// for more than that: append our block (as much of it as was asked).
	if (len == total && wlength > total && len + ACM_DESC_LEN <= MAX_TRANSFER_SIZE) {
		uint8_t block[ACM_DESC_LEN];
		if (acm_build_descriptors(nif, type == USB_DT_CONFIG, block, sizeof(block)) == 0)
			return;
		size_t room = wlength - total;
		size_t n = room < ACM_DESC_LEN ? room : ACM_DESC_LEN;
		memcpy(d + len, block, n);
		io.inner.length = len + n;
	}
}

/*----------------------------------------------------------------------*/
/* ep0                                                                  */

namespace {

bool ep0_ack(int fd)
{
	struct usb_raw_ep_io io;
	io.ep = 0;
	io.flags = 0;
	io.length = 0;
	int rv = usb_raw_ep0_read_try(fd, &io);
	if (rv < 0)
		printf("console: ep0 ack failed: %d\n", rv);
	return rv >= 0;
}

bool ep0_reply(int fd, const void *data, size_t n, uint16_t wlength)
{
	struct usb_raw_transfer_io io;
	io.inner.ep = 0;
	io.inner.flags = 0;
	if (n > wlength)
		n = wlength;
	memcpy(io.data, data, n);
	io.inner.length = n;
	int rv = usb_raw_ep0_write_try(fd, (struct usb_raw_ep_io *)&io);
	if (rv < 0)
		printf("console: ep0 reply failed: %d\n", rv);
	return rv >= 0;
}

void ep0_stall(int fd)
{
	int rv = usb_raw_ep0_stall_try(fd);
	if (rv < 0 && rv != -EBUSY)
		printf("console: ep0 stall failed: %d\n", rv);
}

int ep_num_for_address(uint8_t addr)
{
	if (addr == g_acm.notify_ep.bEndpointAddress)
		return g_acm.notify_num;
	if (addr == g_acm.in_ep.bEndpointAddress)
		return g_acm.in_num;
	if (addr == g_acm.out_ep.bEndpointAddress)
		return g_acm.out_num;
	return -2;
}

} // namespace

bool acm_handle_ep0(int fd, const struct usb_ctrlrequest *ctrl)
{
	if (!g_acm.reserved)
		return false;

	uint8_t recip = ctrl->bRequestType & USB_RECIP_MASK;
	uint8_t type = ctrl->bRequestType & USB_TYPE_MASK;
	bool in = ctrl->bRequestType & USB_DIR_IN;
	uint8_t idx = ctrl->wIndex & 0xff;
	static const uint8_t zero2[2] = {0, 0};

	if (recip == USB_RECIP_INTERFACE) {
		if (g_acm.comm_if == 0xff || (idx != g_acm.comm_if && idx != g_acm.data_if))
			return false;
		if (type == USB_TYPE_STANDARD) {
			switch (ctrl->bRequest) {
			case USB_REQ_GET_STATUS:
				ep0_reply(fd, zero2, 2, ctrl->wLength);
				return true;
			case USB_REQ_GET_INTERFACE:
				ep0_reply(fd, zero2, 1, ctrl->wLength);
				return true;
			case USB_REQ_SET_INTERFACE:
			case USB_REQ_CLEAR_FEATURE:
			case USB_REQ_SET_FEATURE:
				ep0_ack(fd);
				return true;
			default:
				ep0_stall(fd);
				return true;
			}
		}
		if (type == USB_TYPE_CLASS) {
			switch (ctrl->bRequest) {
			case USB_CDC_REQ_SET_LINE_CODING:
				// A control-OUT with a 7-byte data stage. Reading it
				// (EP0_READ) queues an OUT buffer on ep0, and on this musb
				// UDC that is racy: a second SET_LINE_CODING arriving before
				// ep0 is back in the RX stage makes usb_ep_queue return
				// -EINVAL, which raw-gadget turns into STATE_DEV_FAILED and
				// the proxy dies. We advertise bmCapabilities=0 (no line
				// coding), so the correct answer is a STALL -- and a STALL
				// takes the EP0_STALL path, not the racy queue path. We have
				// no use for the baud rate anyway.
				ep0_stall(fd);
				return true;
			case USB_CDC_REQ_GET_LINE_CODING:
				ep0_reply(fd, g_acm.line_coding, 7, ctrl->wLength);
				return true;
			case USB_CDC_REQ_SET_CONTROL_LINE_STATE: {
				bool dtr = ctrl->wValue & 0x01;
				if (dtr != g_acm.dtr || !g_acm.dtr_seen)
					printf("[%.3f] console: host %s the port\n", uptime_s(),
					       dtr ? "opened" : "closed");
				g_acm.dtr = dtr;
				g_acm.dtr_seen = true;
				ep0_ack(fd);
				return true;
			}
			default:
				if (in)
					ep0_stall(fd);
				else
					ep0_ack(fd);
				return true;
			}
		}
		ep0_stall(fd);
		return true;
	}

	if (recip == USB_RECIP_ENDPOINT) {
		int num = ep_num_for_address(idx);
		if (num == -2)
			return false;
		if (type != USB_TYPE_STANDARD) {
			ep0_stall(fd);
			return true;
		}
		switch (ctrl->bRequest) {
		case USB_REQ_GET_STATUS:
			ep0_reply(fd, zero2, 2, ctrl->wLength);
			return true;
		case USB_REQ_CLEAR_FEATURE:
			if (ctrl->wValue == USB_ENDPOINT_HALT && num >= 0)
				usb_raw_ep_clear_halt_try(fd, num);
			ep0_ack(fd);
			return true;
		case USB_REQ_SET_FEATURE:
			if (ctrl->wValue == USB_ENDPOINT_HALT && num >= 0)
				usb_raw_ep_set_halt_try(fd, num);
			ep0_ack(fd);
			return true;
		default:
			ep0_stall(fd);
			return true;
		}
	}

	return false;
}

void acm_class_reset(void)
{
	g_acm.dtr = false;
	g_acm.dtr_seen = false;
	static const uint8_t def[7] = {0x00, 0xc2, 0x01, 0x00, 0x00, 0x00, 0x08};
	memcpy(g_acm.line_coding, def, 7);
}

/*----------------------------------------------------------------------*/
/* Data threads                                                         */

namespace {

// Host -> pty. Blocks in EP_READ until the host writes; SIGUSR1 (EINTR) or a
// reset (ESHUTDOWN) get it out.
void *acm_out_thread(void *)
{
	unblock_sigusr1();
	struct usb_raw_transfer_io io;
	while (!g_acm.stop) {
		io.inner.ep = g_acm.out_num;
		io.inner.flags = 0;
		io.inner.length = 512;
		int rv = usb_raw_ep_read_try(g_acm.fd, (struct usb_raw_ep_io *)&io);
		if (rv < 0) {
			if (rv == -EINTR)
				continue;
			// Reset / disconnect / halted: ep0 will stop us; do not spin.
			usleep(10 * 1000);
			continue;
		}
		int master = shell_master_fd();
		if (master < 0 || rv == 0)
			continue;
		ssize_t w = write(master, io.data, rv);
		if (w < 0)
			g_acm.dropped_out += rv;
		else if (w < rv)
			g_acm.dropped_out += rv - w;
	}
	return nullptr;
}

// pty -> host. Waits on the pty master and the stop pipe; forwards while the
// host holds the port open, drops otherwise. Packets are at most 511 bytes so
// every one is short: no zero-length-packet bookkeeping needed (CDC data has
// no framing anyway).
void *acm_in_thread(void *)
{
	unblock_sigusr1();
	struct usb_raw_transfer_io io;
	while (!g_acm.stop) {
		int master = shell_master_fd();
		struct pollfd pfd[2];
		int n = 0;
		if (master >= 0) {
			pfd[n].fd = master;
			pfd[n].events = POLLIN;
			n++;
		}
		pfd[n].fd = g_acm.stop_pipe[0];
		pfd[n].events = POLLIN;
		n++;
		int pr = poll(pfd, n, master >= 0 ? -1 : 500);
		if (pr <= 0 || g_acm.stop)
			continue;
		if (master < 0 || !(pfd[0].revents & (POLLIN | POLLHUP | POLLERR)))
			continue;

		ssize_t r = read(master, io.data, 511);
		if (r < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			// EIO: no shell on the slave right now (respawning).
			usleep(50 * 1000);
			continue;
		}
		if (r == 0)
			continue;
		if (g_acm.dtr_seen && !g_acm.dtr) {
			g_acm.dropped_in += r;
			continue;
		}
		io.inner.ep = g_acm.in_num;
		io.inner.flags = 0;
		io.inner.length = r;
		int rv = usb_raw_ep_write_try(g_acm.fd, (struct usb_raw_ep_io *)&io);
		if (rv < 0) {
			g_acm.dropped_in += r;
			if (rv != -EINTR)
				usleep(10 * 1000);
		}
	}
	return nullptr;
}

} // namespace

void acm_start(int fd)
{
	if (!g_acm.reserved || g_acm.running)
		return;
	g_acm.fd = fd;

	int rv = usb_raw_ep_enable_try(fd, &g_acm.notify_ep);
	if (rv < 0) {
		printf("console: enabling the notification endpoint failed: %d\n", rv);
		return;
	}
	g_acm.notify_num = rv;
	rv = usb_raw_ep_enable_try(fd, &g_acm.in_ep);
	if (rv < 0) {
		printf("console: enabling bulk IN failed: %d\n", rv);
		usb_raw_ep_disable_try(fd, g_acm.notify_num);
		g_acm.notify_num = -1;
		return;
	}
	g_acm.in_num = rv;
	rv = usb_raw_ep_enable_try(fd, &g_acm.out_ep);
	if (rv < 0) {
		printf("console: enabling bulk OUT failed: %d\n", rv);
		usb_raw_ep_disable_try(fd, g_acm.in_num);
		usb_raw_ep_disable_try(fd, g_acm.notify_num);
		g_acm.in_num = g_acm.notify_num = -1;
		return;
	}
	g_acm.out_num = rv;

	if (pipe(g_acm.stop_pipe) < 0) {
		perror("console: pipe");
		g_acm.stop_pipe[0] = g_acm.stop_pipe[1] = -1;
	}
	g_acm.stop = false;
	pthread_create(&g_acm.th_out, nullptr, acm_out_thread, nullptr);
	pthread_create(&g_acm.th_in, nullptr, acm_in_thread, nullptr);
	g_acm.running = true;
	printf("[%.3f] console: CDC-ACM up (ep #%d/#%d/#%d)\n", uptime_s(),
	       g_acm.notify_num, g_acm.in_num, g_acm.out_num);
}

void acm_stop(int fd)
{
	if (!g_acm.running)
		return;
	g_acm.stop = true;
	if (g_acm.stop_pipe[1] >= 0) {
		char c = 1;
		if (write(g_acm.stop_pipe[1], &c, 1) < 0) { /* ignore */ }
	}
	pthread_kill(g_acm.th_out, SIGUSR1);
	pthread_kill(g_acm.th_in, SIGUSR1);
	pthread_join(g_acm.th_out, nullptr);
	pthread_join(g_acm.th_in, nullptr);
	g_acm.th_out = g_acm.th_in = 0;

	for (int *num : {&g_acm.out_num, &g_acm.in_num, &g_acm.notify_num}) {
		if (*num >= 0) {
			int rv = usb_raw_ep_disable_try(fd, *num);
			if (rv < 0 && rv != -ESHUTDOWN && verbose_level)
				printf("console: ep #%d disable: %d\n", *num, rv);
		}
		*num = -1;
	}
	for (int &p : g_acm.stop_pipe) {
		if (p >= 0)
			close(p);
		p = -1;
	}
	g_acm.dtr = false;
	g_acm.running = false;
	printf("[%.3f] console: CDC-ACM down (dropped %llu in / %llu out bytes)\n",
	       uptime_s(), (unsigned long long)g_acm.dropped_in,
	       (unsigned long long)g_acm.dropped_out);
	g_acm.dropped_in = g_acm.dropped_out = 0;
}

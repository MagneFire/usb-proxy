// CDC-ACM "USB console": a serial function that usb-proxy adds to the gadget
// it presents, so the appliance's shell is reachable over the same USB port
// that carries the proxied device (no UART dongle needed).
//
// raw-gadget owns the whole UDC, so this cannot be a configfs/g_serial gadget
// next to usb-proxy; the function lives in-process. Two uses:
//   - composite mode (proxy.cpp): appended to the proxied device's own
//     configuration -- descriptors spliced into the forwarded GET_DESCRIPTOR
//     replies, ep0 requests for the ACM interfaces answered locally, the three
//     endpoints enabled on SET_CONFIGURATION;
//   - idle mode (gadget-idle.cpp): the only function of a small standalone
//     gadget attached while usb-proxy waits for a device.
// The bytes go to/from a pty whose other end runs a shell (console-shell.cpp).
#ifndef CONSOLE_ACM_H
#define CONSOLE_ACM_H

#include <atomic>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <vector>

#include <linux/usb/ch9.h>

#include "host-raw-gadget.h"

// Size of the descriptor block acm_build_descriptors() emits: IAD(8) + comm
// interface(9) + header(5) + call management(5) + ACM(4) + union(5) +
// notification endpoint(7) + data interface(9) + bulk IN(7) + bulk OUT(7).
#define ACM_DESC_LEN 66
// Interfaces the function adds to a configuration.
#define ACM_NUM_INTERFACES 2

struct AcmFunction {
	// Endpoints reserved on the UDC -> the function is part of the gadget.
	bool reserved = false;
	// Interface numbers in the current configuration. 0xff = not known yet
	// (composite mode learns them from the device's config descriptor; before
	// that nothing may be answered locally, or the proxied device's own
	// interfaces 0/1 would be shadowed).
	uint8_t comm_if = 0xff;
	uint8_t data_if = 0xff;
	// Gadget-side endpoint descriptors (high-speed sizes).
	struct usb_endpoint_descriptor notify_ep = {};
	struct usb_endpoint_descriptor in_ep = {};
	struct usb_endpoint_descriptor out_ep = {};
	// raw-gadget endpoint handles while enabled, else -1.
	int notify_num = -1;
	int in_num = -1;
	int out_num = -1;
	// CDC line coding as last set by the host (115200 8N1 default).
	uint8_t line_coding[7] = {0x00, 0xc2, 0x01, 0x00, 0x00, 0x00, 0x08};
	// Host has the port open (SET_CONTROL_LINE_STATE DTR). Output is
	// discarded while it is not, so a closed port never stalls the shell.
	std::atomic<bool> dtr{false};
	// Whether the host has ever sent SET_CONTROL_LINE_STATE this session. A
	// host that never does still gets output (the write then blocks until it
	// reads, like a UART with flow control); one that does gets output only
	// while DTR is up.
	std::atomic<bool> dtr_seen{false};

	std::atomic<bool> stop{false};
	bool running = false;
	pthread_t th_in = 0;
	pthread_t th_out = 0;
	int stop_pipe[2] = {-1, -1};
	int fd = -1;
	// Bytes dropped because the port was closed / the pty was full.
	uint64_t dropped_in = 0;
	uint64_t dropped_out = 0;
};

extern AcmFunction g_acm;

// Serialize the ACM descriptor block for interfaces comm_if/comm_if+1 using
// the reserved endpoint addresses. high_speed selects 512-byte bulk packets
// (CONFIG) versus 64-byte ones (OTHER_SPEED_CONFIG). Returns bytes written
// (ACM_DESC_LEN) or 0 if cap is too small or nothing is reserved.
size_t acm_build_descriptors(uint8_t comm_if, bool high_speed,
			     uint8_t *out, size_t cap);

// Pick three UDC endpoints (INT IN, BULK IN, BULK OUT) from the tail of the
// EPS_INFO list, so a proxied device keeps the low numbers, and mark them in
// `reserved` (sized num). Fills g_acm's endpoint descriptors. Returns false
// (and clears g_acm.reserved) when the pool cannot fit the function.
bool acm_reserve_endpoints(const struct usb_raw_eps_info &info, int num,
			   std::vector<bool> &reserved);
void acm_unreserve(void);

// Composite mode: patch a forwarded GET_DESCRIPTOR reply in place. DEVICE:
// class 00/00/00 becomes EF/02/01 (misc, IAD). CONFIG/OTHER_SPEED_CONFIG:
// bNumInterfaces += 2, wTotalLength += ACM_DESC_LEN, and the block is
// appended when the device returned its whole descriptor. `io` holds the
// reply; wlength is the host's request length (the reply never exceeds it).
void acm_rewrite_descriptor(const struct usb_ctrlrequest *ctrl,
			    struct usb_raw_transfer_io &io, uint16_t wlength);

// Answer an ep0 request whose recipient is one of the ACM interfaces or
// endpoints (standard SET/GET_INTERFACE, GET_STATUS, HALT features; CDC
// SET/GET_LINE_CODING, SET_CONTROL_LINE_STATE). Returns true if the request
// was ours and has been acked/answered/stalled; false to let the caller
// forward it to the proxied device.
bool acm_handle_ep0(int fd, const struct usb_ctrlrequest *ctrl);

// Enable the three endpoints and start the pty<->USB threads / stop them and
// disable the endpoints. Both idempotent; safe when nothing is reserved.
void acm_start(int fd);
void acm_stop(int fd);
// Host reset: forget DTR and the line coding.
void acm_class_reset(void);

#endif /* CONSOLE_ACM_H */

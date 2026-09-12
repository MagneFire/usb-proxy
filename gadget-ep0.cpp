#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "gadget-ep0.h"
#include "host-raw-gadget.h"

bool ep0_reply(int fd, const char *tag, const void *data, size_t n, uint16_t wlength)
{
	struct usb_raw_transfer_io io;
	io.inner.ep = 0;
	io.inner.flags = 0;
	if (n > wlength)
		n = wlength;
	if (n > sizeof(io.data))
		n = sizeof(io.data);
	memcpy(io.data, data, n);
	io.inner.length = n;
	int rv = usb_raw_ep0_write_try(fd, (struct usb_raw_ep_io *)&io);
	if (rv < 0)
		printf("%s: ep0 reply failed: %d\n", tag, rv);
	return rv >= 0;
}

bool ep0_ack(int fd, const char *tag)
{
	struct usb_raw_ep_io io;
	io.ep = 0;
	io.flags = 0;
	io.length = 0;
	int rv = usb_raw_ep0_read_try(fd, &io);
	if (rv < 0)
		printf("%s: ep0 ack failed: %d\n", tag, rv);
	return rv >= 0;
}

void ep0_stall(int fd, const char *tag)
{
	int rv = usb_raw_ep0_stall_try(fd);
	if (rv < 0 && rv != -EBUSY)
		printf("%s: ep0 stall failed: %d\n", tag, rv);
}

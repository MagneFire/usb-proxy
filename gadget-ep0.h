// ep0 replies for the functions usb-proxy answers locally (the CDC-ACM
// console, the persistent gadget): the raw-gadget control transfer patterns
// in one place, non-exiting, with a log tag for the failure line.
#ifndef GADGET_EP0_H
#define GADGET_EP0_H

#include <stddef.h>
#include <stdint.h>

// Control-IN data stage: write min(n, wLength) bytes.
bool ep0_reply(int fd, const char *tag, const void *data, size_t n, uint16_t wlength);
// Control-OUT with no data: a zero-length EP0_READ acks the status stage.
bool ep0_ack(int fd, const char *tag);
// Protocol STALL. -EBUSY (already stalled) is not worth a line.
void ep0_stall(int fd, const char *tag);

#endif /* GADGET_EP0_H */

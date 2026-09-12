// Idle console: while usb-proxy waits for a device to proxy there is no gadget
// on the bus at all, so nothing to type into. This attaches a small standalone
// gadget whose only function is the CDC-ACM console (console-acm.cpp); it is
// torn down the moment a device appears so the composite can take the UDC.
#ifndef GADGET_IDLE_H
#define GADGET_IDLE_H

// Attach the console-only gadget on the given UDC (fails open: logs and
// returns false, the wait for a device continues without a console).
bool idle_gadget_start(const char *driver, const char *device);
// Detach it (no-op when not attached) and stamp the detach time.
void idle_gadget_stop(void);
bool idle_gadget_attached(void);
// Before attaching any other gadget: keep D+ released for at least min_off_ms
// since the last detach, so the host registers the disconnect (macOS misses
// a re-attach that follows within milliseconds).
void idle_gadget_wait_min_off(int min_off_ms);

#endif /* GADGET_IDLE_H */

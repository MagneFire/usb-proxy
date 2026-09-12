// Persistent gadget: one gadget with a fixed identity and a fixed
// configuration -- the CDC-ACM console (console-acm.cpp) plus an adb and a
// fastboot interface (bridge.cpp) -- attached for the life of the process and
// enumerated by the host exactly once. ep0 is answered entirely locally; the
// proxied device is only ever touched through the bulk bridge. The host never
// sees a re-enumeration when the device appears, leaves or changes mode.
//
// This generalises gadget-idle.cpp (which stays as is for the transparent
// mode's console-only idle gadget; the two deliberately do not share code so
// the transparent path is untouched).
#ifndef GADGET_FIXED_H
#define GADGET_FIXED_H

// Bind the fixed gadget to the UDC (retrying EBUSY briefly while a previous
// instance unbinds), reserve endpoints, start the ep0 thread. Returns false
// (after logging) when the UDC cannot be claimed or the endpoints do not fit.
bool fixed_gadget_start(const char *driver, const char *device);
// Stop the ep0 thread, the bridge threads and the console; release the UDC
// (the host sees a disconnect).
void fixed_gadget_stop(void);

// usb-proxy.cpp: assign UDC endpoints to host_device_desc (and reserve the
// console's) once the raw-gadget fd is running. Shared with the transparent
// path.
int remap_host_endpoints_if_needed(int fd);

#endif /* GADGET_FIXED_H */

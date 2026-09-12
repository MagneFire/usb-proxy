#include "host-raw-gadget.h"

void ep0_loop(int fd);

// Bounded drain of the IN queues, then _exit(0). The one way out once the
// proxied device is gone; safe to call from any thread (see proxy.cpp).
void drain_in_queues_and_exit(void);

// The proxied device is gone (a NO_DEVICE from libusb, its devtmpfs node
// vanished, or the hotplug callback fired). Transparent mode: this is
// drain_in_queues_and_exit(). Persistent-gadget mode: it flags the loss for the
// device manager (bridge.h) and returns, and the caller must leave its loop.
void device_lost(const char *where);

// Wait (bounded, ~300 ms) for the bulk/interrupt IN queues of the current
// configuration to empty, so a device's last answer (the `fastboot boot` OKAY)
// still reaches the host. Returns true if they did.
bool drain_in_queues(void);

// The endpoint machinery of one altsetting, split so the persistent gadget can
// keep endpoints enabled across device binds:
//   eps_enable    -> USB_RAW_IOCTL_EP_ENABLE for every endpoint (ep_num set)
//   eps_start     -> queues, peer links, the read/write threads
//   eps_stop      -> stop + join the threads, free the queues
//   eps_disable   -> USB_RAW_IOCTL_EP_DISABLE
// process_eps() = enable + start; terminate_eps() = stop + disable.
void eps_enable(int fd, struct raw_gadget_altsetting *alt);
void eps_start(int fd, struct raw_gadget_altsetting *alt);
void eps_stop(struct raw_gadget_altsetting *alt);
void eps_disable(int fd, struct raw_gadget_altsetting *alt);
void process_eps(int fd, int config, int interface, int altsetting);
void terminate_eps(int fd, int config, int interface, int altsetting);

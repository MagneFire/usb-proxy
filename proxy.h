void ep0_loop(int fd);

// Bounded drain of the IN queues, then _exit(0). The one way out once the
// proxied device is gone; safe to call from any thread (see proxy.cpp).
void drain_in_queues_and_exit(void);

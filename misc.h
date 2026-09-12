#ifndef USBPROXY_MISC_H
#define USBPROXY_MISC_H

#include <assert.h>
#include <atomic>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <stdio.h>
#include <string>
#include <getopt.h>
#include <signal.h>
#include <chrono>
#include <sys/stat.h>
#include <linux/usb/ch9.h>
#include <jsoncpp/json/json.h>

extern int verbose_level;
extern bool please_stop_ep0;
extern std::atomic<bool> please_stop_eps;

extern bool injection_enabled;
extern std::string injection_file;
extern Json::Value injection_config;

extern bool customized_config_enabled;
extern bool reset_device_before_proxy;
extern bool bmaxpacketsize0_must_greater_than_64;
extern int iso_batch_size;
extern int bulk_out_max_in_flight;
extern bool adb_ack_accel;
// Bulk-OUT packets per gadget read on musb (1 = the one-packet clamp).
extern int musb_out_read_packets;
#define MUSB_OUT_READ_PACKETS_MAX 64
extern bool gadget_is_musb;

// Persistent gadget (gadget-fixed.cpp, bridge.cpp): one fixed gadget for the
// life of the process, the proxied device's bulk endpoints bridged onto it.
// It carries the CDC-ACM console (console-acm.cpp) whose pty runs
// usb_console_shell.
extern bool persistent_gadget;
extern int gadget_vendor_id;
extern int gadget_product_id;
extern std::string gadget_serial;
extern std::string usb_console_shell;

// Installed for SIGUSR1: interrupts a blocking raw-gadget ioctl, nothing else.
void noop_signal_handler(int);

// Block every asynchronous signal on the calling thread except SIGINT/SIGTERM
// (a real shutdown) and the synchronous fault signals; with keep_sigusr1 the
// thread stays interruptible by pthread_kill(SIGUSR1) as well. For threads
// that sleep in raw-gadget ioctls, which any stray signal would pop out of
// with EINTR.
void block_incidental_signals(bool keep_sigusr1);

// Threads created from one that blocked SIGUSR1 (pthread_create copies the
// mask) call this so pthread_kill(SIGUSR1) can still pop them out of an ioctl.
void unblock_sigusr1(void);

// Seconds since boot (CLOCK_MONOTONIC), the same clock dmesg stamps with, so
// milestone log lines can be lined up with the kernel's USB events.
double uptime_s(void);

std::string hexToAscii(std::string input);
int hexToDecimal(int input);

#endif /* USBPROXY_MISC_H */

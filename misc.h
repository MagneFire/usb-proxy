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
extern bool drop_zero_len_out;
extern bool adb_bulk_diag;
extern bool adb_ack_accel;
extern int musb_out_read_packets;
extern bool gadget_is_musb;

// CDC-ACM console over the gadget port (console-acm.cpp, gadget-idle.cpp).
extern bool usb_console;
extern bool usb_console_idle;
extern int usb_console_idle_delay_ms;
extern int usb_console_min_off_ms;
extern std::string usb_console_shell;
// UDC the gadget binds to (--driver / --device), for the idle gadget.
extern const char *gadget_driver;
extern const char *gadget_device;

// Installed for SIGUSR1: interrupts a blocking raw-gadget ioctl, nothing else.
void noop_signal_handler(int);

// Seconds since boot (CLOCK_MONOTONIC), the same clock dmesg stamps with, so
// milestone log lines can be lined up with the kernel's USB events.
double uptime_s(void);

std::string hexToAscii(std::string input);
int hexToDecimal(int input);

#endif /* USBPROXY_MISC_H */

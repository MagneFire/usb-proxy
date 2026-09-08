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

// Seconds since boot (CLOCK_MONOTONIC), the same clock dmesg stamps with, so
// milestone log lines can be lined up with the kernel's USB events.
double uptime_s(void);

std::string hexToAscii(std::string input);
int hexToDecimal(int input);

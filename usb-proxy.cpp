#include <errno.h>
#include <atomic>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <sys/prctl.h>

#include "host-raw-gadget.h"
#include "device-libusb.h"
#include "proxy.h"
#include "misc.h"
#include "power-policy.h"
#include "console-acm.h"
#include "console-shell.h"
#include "gadget-idle.h"
#include "gadget-fixed.h"
#include "bridge.h"

int verbose_level = 0;
bool please_stop_ep0 = false;
std::atomic<bool> please_stop_eps(false);

bool injection_enabled = false;
std::string injection_file = "injection.json";
Json::Value injection_config;

bool customized_config_enabled = false;
std::string customized_config_file = "config.json";
bool reset_device_before_proxy = true;
bool bmaxpacketsize0_must_greater_than_64 = true;
bool auto_remap_endpoints = false;
int iso_batch_size = ISO_BATCH_SIZE_DEFAULT;
int bulk_out_max_in_flight = BULK_OUT_IN_FLIGHT_DEFAULT;
// Drop zero-length OUT reads (host bulk transfer-terminator ZLPs) instead of
// forwarding them to the device. On musb the OUT read is clamped to one packet,
// so each read is re-chunked into its own libusb transfer; forwarding a ZLP then
// injects a spurious 0-length transfer between length-framed protocol messages
// (e.g. ADB), desyncing the device. Length-framed protocols don't need the ZLP.
bool drop_zero_len_out = false;
bool adb_bulk_diag = false;
// ADB ACK accelerator: locally acknowledge host WRTEs and swallow the device's
// real OKAYs, hiding the proxy's store-and-forward hop from ADB's one-WRTE-
// in-flight flow control. Opt-in; see the AdbAckAccel comment in proxy.cpp.
bool adb_ack_accel = false;
// Bulk-OUT gadget reads on musb: packets per read buffer (maxp * N, capped to
// MAX_TRANSFER_SIZE). Default 1 = the historical one-packet clamp, required on
// kernels without the musb requeue-flush fix. With a fixed kernel, 8 cuts the
// per-packet ioctl overhead substantially.
int musb_out_read_packets = 1;
bool gadget_is_musb = false;
enum usb_device_speed device_speed = USB_SPEED_HIGH;
// CDC-ACM console over the gadget port. Off by default: it changes what the
// host sees (two extra interfaces on the proxied device's configuration), so
// it is a per-deployment choice, not a proxying default.
bool usb_console = false;
bool usb_console_idle = false;
int usb_console_idle_delay_ms = 2000;
int usb_console_min_off_ms = 1000;
std::string usb_console_shell = "/bin/sh -l";
const char *gadget_driver = "dummy_udc";
const char *gadget_device = "dummy_udc.0";
// Persistent gadget (gadget-fixed.cpp / bridge.cpp). Off by default: the
// host then sees usb-proxy's fixed identity, not the proxied device's.
bool persistent_gadget = false;
int gadget_vendor_id = 0x1d6b;
int gadget_product_id = 0x0104;
std::string gadget_serial = "USBPROXY01";

// Print the transform summary for a single injection rule.
// Returns true if the rule references a Lua script_file.
static bool print_rule_transforms(const Json::Value &rule)
{
	bool has_pattern = rule["content_pattern"].size() > 0 &&
			   !rule.get("replacement", "").asString().empty();
	bool has_ops     = rule.isMember("operations") &&
			   rule["operations"].size() > 0;
	bool has_script  = rule.isMember("script_file") &&
			   !rule["script_file"].asString().empty();

	if (has_pattern) printf("  pattern+replace");
	if (has_ops)     printf("  %u operation(s)", rule["operations"].size());
	if (has_script)  printf("  script: %s", rule["script_file"].asString().c_str());
	if (!has_pattern && !has_ops && !has_script) printf("  (no transform configured)");

	return has_script;
}

static void print_injection_summary()
{
	const std::vector<std::string> ep_types   = {"int", "bulk", "isoc"};
	const std::vector<std::string> ctrl_types = {"modify", "ignore", "stall"};

	int active = 0;
	for (const auto &type : ep_types)
		for (unsigned int i = 0; i < injection_config[type].size(); i++)
			if (injection_config[type][i]["enable"].asBool()) active++;
	for (const auto &sub : ctrl_types)
		for (unsigned int i = 0; i < injection_config["control"][sub].size(); i++)
			if (injection_config["control"][sub][i]["enable"].asBool()) active++;

	if (active == 0) {
		printf("Injection rules: none enabled\n");
		return;
	}

	printf("Injection rules: %d active\n", active);

	bool any_script = false;

	for (const auto &type : ep_types) {
		for (unsigned int i = 0; i < injection_config[type].size(); i++) {
			const Json::Value &rule = injection_config[type][i];
			if (!rule["enable"].asBool()) continue;

			int ep = hexToDecimal(rule["ep_address"].asInt());
			printf("  [%-4s]  EP 0x%02x", type.c_str(), ep);
			any_script |= print_rule_transforms(rule);
			printf("\n");
		}
	}

	for (const auto &sub : ctrl_types) {
		for (unsigned int i = 0; i < injection_config["control"][sub].size(); i++) {
			const Json::Value &rule = injection_config["control"][sub][i];
			if (!rule["enable"].asBool()) continue;

			printf("  [control/%-6s]  bRequestType=0x%02x bRequest=0x%02x",
			       sub.c_str(),
			       rule["bRequestType"].asInt(),
			       rule["bRequest"].asInt());

			if (sub == "modify")
				any_script |= print_rule_transforms(rule);
			printf("\n");
		}
	}

#ifndef HAVE_LUA
	if (any_script)
		fprintf(stderr, "Warning: one or more rules use 'script_file' but usb-proxy was built "
			"without Lua support — scripts will be ignored.\n"
			"  Install a Lua dev package and rebuild: make clean && make\n");
#else
	(void)any_script;
#endif
}

void usage() {
	printf("Usage:\n");
	printf("\t-h/--help: print this help message\n");
	printf("\t-v/--verbose: increase verbosity\n");
	printf("\t--device: use specific device\n");
	printf("\t--driver: use specific driver\n");
	printf("\t--vendor_id: use specific vendor_id of USB device\n");
	printf("\t--product_id: use specific product_id of USB device\n");
	printf("\t--enable_injection: enable injection using the default injection.json\n");
	printf("\t--injection_file: enable injection using the specified rules file\n");
	printf("\t--enable_customized_config: enable the customized config feature\n");
	printf("\t--auto_remap_endpoints: enable endpoint remapping when UDC can't use descriptors directly\n");
	printf("\t--iso_batch_size N: number of isochronous packets per transfer (1-%d, default %d)\n",
		ISO_BATCH_SIZE_MAX, ISO_BATCH_SIZE_DEFAULT);
	printf("\t--bulk_out_in_flight N: async bulk-OUT transfers kept in flight (0=synchronous, max %d, default %d)\n\n",
		BULK_OUT_IN_FLIGHT_MAX, BULK_OUT_IN_FLIGHT_DEFAULT);
	printf("\t--adb_bulk_diag: log ADB/file-sync DATA progress on bulk OUT (diagnostic only)\n");
	printf("\t--adb_ack_accel: acknowledge ADB WRTEs locally in both directions (hides the proxy\n");
	printf("\t                 hop from ADB flow control on push AND pull; the file-sync\n");
	printf("\t                 DONE handshake stays end-to-end)\n");
	printf("\t--musb_out_read_packets N: bulk-OUT packets per gadget read on musb (default 1;\n");
	printf("\t                           >1 needs a kernel with the musb requeue-flush fix)\n");
	printf("\t--power_hook PATH: run `PATH active` on the first traffic after an idle period\n");
	printf("\t                   and `PATH idle` once traffic stops, so the board can wind\n");
	printf("\t                   its CPU down while nothing is happening (default: disabled)\n");
	printf("\t--power_idle_ms N: quiet period before `power_hook idle` runs (default %d)\n",
		power_idle_ms);
	printf("\t--settle_ms N: a newly appeared device must survive N ms before the gadget\n");
	printf("\t               attaches (skips transient re-enumerations; default 0 = at once)\n");
	printf("\t--usb_console: add a CDC-ACM serial console (a root shell on a pty) to the\n");
	printf("\t               gadget, next to the proxied device's interfaces; needs\n");
	printf("\t               --auto_remap_endpoints and 3 free UDC endpoints\n");
	printf("\t--usb_console_idle: while no device is attached, present a console-only gadget\n");
	printf("\t--usb_console_idle_delay_ms N: how long the bus must be empty before the idle\n");
	printf("\t               console attaches (default %d)\n", usb_console_idle_delay_ms);
	printf("\t--usb_console_shell CMD: what runs on the console pty (default `%s`)\n",
		usb_console_shell.c_str());
	printf("\t--min_off_ms N: minimum time the gadget stays detached between the idle console\n");
	printf("\t               and the proxied device (default %d)\n", usb_console_min_off_ms);
	printf("\t--persistent_gadget: present ONE fixed gadget for the life of the process (console +\n");
	printf("\t               an adb + a fastboot interface, usb-proxy's own identity) and bridge the\n");
	printf("\t               proxied device's bulk endpoints onto it: the host never re-enumerates\n");
	printf("\t               when the device appears, leaves or switches adb<->fastboot. Devices\n");
	printf("\t               that are not adb/fastboot are ignored while they are attached.\n");
	printf("\t--gadget_vendor_id HEX / --gadget_product_id HEX / --gadget_serial STR: the fixed\n");
	printf("\t               gadget's identity (default %04x:%04x, %s)\n",
		gadget_vendor_id, gadget_product_id, gadget_serial.c_str());
	printf("* If `device` not specified, `usb-proxy` will use `dummy_udc.0` as default device.\n");
	printf("* If `driver` not specified, `usb-proxy` will use `dummy_udc` as default driver.\n");
	printf("* If both `vendor_id` and `product_id` not specified, `usb-proxy` will connect\n");
	printf("  the first USB device it can find.\n");
	printf("* If `injection_file` not specified, `usb-proxy` will use `injection.json` by default.\n\n");
	exit(1);
}

void handle_signal(int signum) {
	switch (signum) {
	case SIGTERM:
	case SIGINT:
		static bool signal_received = false;
		if (signal_received) {
			printf("Signal received again, force exiting\n");
			exit(1);
		}
		if (signum == SIGTERM)
			printf("Received SIGTERM, stopping...\n");
		else
			printf("Received SIGINT, stopping...\n");

		signal_received = true;
		please_stop_ep0 = true;
		please_stop_eps = true;
		break;
	}
}

// Wrapper for UDC endpoint info, allowing future extension with additional state.
struct EndpointCandidate {
	struct usb_raw_ep_info info;
	// Taken by the CDC-ACM console; never handed to a proxied endpoint.
	bool reserved = false;
};

static bool candidate_supports_endpoint(const EndpointCandidate &candidate,
					const struct usb_endpoint_descriptor &endpoint)
{
	bool dir_in = usb_endpoint_dir_in(&endpoint);
	int type = usb_endpoint_type(&endpoint);

	if (dir_in && !candidate.info.caps.dir_in)
		return false;
	if (!dir_in && !candidate.info.caps.dir_out)
		return false;

	switch (type) {
	case USB_ENDPOINT_XFER_ISOC:
		if (!candidate.info.caps.type_iso)
			return false;
		break;
	case USB_ENDPOINT_XFER_BULK:
		if (!candidate.info.caps.type_bulk)
			return false;
		break;
	case USB_ENDPOINT_XFER_INT:
		if (!candidate.info.caps.type_int)
			return false;
		break;
	default:
		return false;
	}

	uint16_t max_packet = usb_endpoint_maxp(&endpoint);
	if (candidate.info.limits.maxpacket_limit &&
	    max_packet > candidate.info.limits.maxpacket_limit)
		return false;

	return true;
}

static uint8_t compute_host_endpoint_address(const EndpointCandidate &candidate,
					     uint8_t device_address,
					     bool dir_in)
{
	if (candidate.info.addr == USB_RAW_EP_ADDR_ANY)
		return device_address;

	uint8_t host_address = static_cast<uint8_t>(candidate.info.addr);
	if (dir_in)
		host_address |= USB_DIR_IN;
	else
		host_address &= ~USB_DIR_IN;

	return host_address;
}

static int find_candidate_index(const std::vector<EndpointCandidate> &candidates,
				std::vector<bool> &candidate_used,
				const struct usb_endpoint_descriptor &endpoint)
{
	for (size_t i = 0; i < candidates.size(); i++) {
		if (candidate_used[i] || candidates[i].reserved)
			continue;
		if (!candidate_supports_endpoint(candidates[i], endpoint))
			continue;
		return i;
	}
	return -1;
}

static int remap_config_endpoints(struct raw_gadget_config *config,
				  const std::vector<EndpointCandidate> &candidates)
{
	std::vector<bool> candidate_used(candidates.size(), false);
	std::unordered_map<uint8_t, size_t> device_to_candidate;

	for (int i = 0; i < config->config.bNumInterfaces; i++) {
		struct raw_gadget_interface *iface = &config->interfaces[i];
		for (int j = 0; j < iface->num_altsettings; j++) {
			struct raw_gadget_altsetting *alt = &iface->altsettings[j];
			uint8_t iface_num = alt->interface.bInterfaceNumber;
			uint8_t alt_setting = alt->interface.bAlternateSetting;
			for (int k = 0; k < alt->interface.bNumEndpoints; k++) {
				struct raw_gadget_endpoint *ep = &alt->endpoints[k];
				uint8_t device_address = ep->device_bEndpointAddress;
				auto existing = device_to_candidate.find(device_address);

				size_t candidate_index;
				if (existing != device_to_candidate.end()) {
					candidate_index = existing->second;
				}
				else {
					int idx = find_candidate_index(candidates,
								       candidate_used,
								       ep->endpoint);
					if (idx < 0) {
						printf("Failed to remap endpoint 0x%02x "
						       "(interface %u, alt %u)\n",
						       device_address, iface_num, alt_setting);
						return -1;
					}
					candidate_index = (size_t)idx;
					device_to_candidate[device_address] = candidate_index;
					candidate_used[candidate_index] = true;
				}

				bool dir_in = usb_endpoint_dir_in(&ep->endpoint);
				uint8_t host_address = compute_host_endpoint_address(
					candidates[candidate_index], device_address, dir_in);

				if (host_address != ep->endpoint.bEndpointAddress) {
					printf("Remapping endpoint 0x%02x -> 0x%02x "
					       "(interface %u, alt %u)\n",
						device_address, host_address, iface_num, alt_setting);
				}

				ep->endpoint.bEndpointAddress = host_address;
				ep->udc_maxpacket_limit = candidates[candidate_index].info.limits.maxpacket_limit;

				// Clamp isochronous max packet size to UDC limit; UDC can't do high bandwidth.
				if (usb_endpoint_type(&ep->endpoint) == USB_ENDPOINT_XFER_ISOC &&
				    ep->udc_maxpacket_limit) {
					uint16_t maxp = usb_endpoint_maxp(&ep->endpoint);
					uint16_t base = maxp & 0x7ff;
					if (base > ep->udc_maxpacket_limit ||
					    (ep->endpoint.wMaxPacketSize & 0x1800)) {
						ep->endpoint.wMaxPacketSize = ep->udc_maxpacket_limit;
					}
				}
			}
		}
	}

	return 0;
}

static int remap_all_configs(const std::vector<EndpointCandidate> &candidates)
{
	for (int i = 0; i < host_device_desc.device.bNumConfigurations; i++) {
		if (remap_config_endpoints(&host_device_desc.configs[i], candidates) < 0)
			return -1;
	}
	return 0;
}

int remap_host_endpoints_if_needed(int fd)
{
	if (!auto_remap_endpoints) {
		if (usb_console)
			printf("console: --usb_console needs --auto_remap_endpoints "
			       "(the console takes UDC endpoints a proxied device could "
			       "address by number); console disabled\n");
		return 0;
	}

	struct usb_raw_eps_info eps_info;
	memset(&eps_info, 0, sizeof(eps_info));

	int num = usb_raw_eps_info(fd, &eps_info);
	if (num <= 0) {
		printf("Failed to fetch endpoint info for remapping\n");
		return -1;
	}

	std::vector<EndpointCandidate> candidates;
	for (int i = 0; i < num; i++) {
		EndpointCandidate candidate;
		candidate.info = eps_info.eps[i];
		candidates.push_back(candidate);
	}

	// The console takes its endpoints first, from the tail of the pool, so
	// the proxied device keeps the low numbers. If the device then does not
	// fit, the console gives way: proxying is the job, the console a bonus.
	if (usb_console) {
		std::vector<bool> reserved(num, false);
		if (acm_reserve_endpoints(eps_info, num, reserved)) {
			for (int i = 0; i < num; i++)
				candidates[i].reserved = reserved[i];
			if (remap_all_configs(candidates) == 0)
				return 0;
			printf("console: device endpoints do not fit next to the console; "
			       "console disabled for this device\n");
			acm_unreserve();
			for (int i = 0; i < num; i++)
				candidates[i].reserved = false;
		}
	}

	return remap_all_configs(candidates);
}

int setup_host_usb_desc() {
	host_device_desc.device = {
		.bLength =		device_device_desc.bLength,
		.bDescriptorType =	device_device_desc.bDescriptorType,
		.bcdUSB =		device_device_desc.bcdUSB,
		.bDeviceClass =		device_device_desc.bDeviceClass,
		.bDeviceSubClass =	device_device_desc.bDeviceSubClass,
		.bDeviceProtocol =	device_device_desc.bDeviceProtocol,
		.bMaxPacketSize0 =	device_device_desc.bMaxPacketSize0,
		.idVendor =		device_device_desc.idVendor,
		.idProduct =		device_device_desc.idProduct,
		.bcdDevice =		device_device_desc.bcdDevice,
		.iManufacturer =	device_device_desc.iManufacturer,
		.iProduct =		device_device_desc.iProduct,
		.iSerialNumber =	device_device_desc.iSerialNumber,
		.bNumConfigurations =	device_device_desc.bNumConfigurations,
	};

	int bNumConfigurations = device_device_desc.bNumConfigurations;
	host_device_desc.configs = new struct raw_gadget_config[bNumConfigurations];
	for (int i = 0; i < bNumConfigurations; i++) {
		struct usb_config_descriptor temp_config = {
			.bLength =		device_config_desc[i]->bLength,
			.bDescriptorType =	device_config_desc[i]->bDescriptorType,
			.wTotalLength =		device_config_desc[i]->wTotalLength,
			.bNumInterfaces =	device_config_desc[i]->bNumInterfaces,
			.bConfigurationValue =	device_config_desc[i]->bConfigurationValue,
			.iConfiguration = 	device_config_desc[i]->iConfiguration,
			.bmAttributes =		device_config_desc[i]->bmAttributes,
			.bMaxPower =		device_config_desc[i]->MaxPower,
		};
		host_device_desc.configs[i].config = temp_config;

		int bNumInterfaces = device_config_desc[i]->bNumInterfaces;
		struct raw_gadget_interface *temp_interfaces =
			new struct raw_gadget_interface[bNumInterfaces];
		for (int j = 0; j < bNumInterfaces; j++) {
			int num_altsetting = device_config_desc[i]->interface[j].num_altsetting;
			struct raw_gadget_altsetting *temp_altsettings =
				new struct raw_gadget_altsetting[num_altsetting];
			for (int k = 0; k < num_altsetting; k++) {
				const struct libusb_interface_descriptor temp_device_altsetting =
					device_config_desc[i]->interface[j].altsetting[k];
				struct usb_interface_descriptor temp_host_altsetting = {
					.bLength =		temp_device_altsetting.bLength,
					.bDescriptorType =	temp_device_altsetting.bDescriptorType,
					.bInterfaceNumber =	temp_device_altsetting.bInterfaceNumber,
					.bAlternateSetting =	temp_device_altsetting.bAlternateSetting,
					.bNumEndpoints =	temp_device_altsetting.bNumEndpoints,
					.bInterfaceClass =	temp_device_altsetting.bInterfaceClass,
					.bInterfaceSubClass =	temp_device_altsetting.bInterfaceSubClass,
					.bInterfaceProtocol =	temp_device_altsetting.bInterfaceProtocol,
					.iInterface =		temp_device_altsetting.iInterface,
				};
				temp_altsettings[k].interface = temp_host_altsetting;

				if (!temp_device_altsetting.bNumEndpoints) {
					printf("InterfaceNumber %x AlternateSetting %x has no endpoint, skip\n",
						temp_device_altsetting.bInterfaceNumber,
						temp_device_altsetting.bAlternateSetting);
					temp_altsettings[k].endpoints = NULL;
					continue;
				}

				int bNumEndpoints = temp_device_altsetting.bNumEndpoints;
				struct raw_gadget_endpoint *temp_endpoints =
					new struct raw_gadget_endpoint[bNumEndpoints];
				for (int l = 0; l < bNumEndpoints; l++) {
					struct usb_endpoint_descriptor temp_endpoint = {
						.bLength =		temp_device_altsetting.endpoint[l].bLength,
						.bDescriptorType =	temp_device_altsetting.endpoint[l].bDescriptorType,
						.bEndpointAddress =	temp_device_altsetting.endpoint[l].bEndpointAddress,
						.bmAttributes =		temp_device_altsetting.endpoint[l].bmAttributes,
						.wMaxPacketSize =	temp_device_altsetting.endpoint[l].wMaxPacketSize,
						.bInterval =		temp_device_altsetting.endpoint[l].bInterval,
						.bRefresh =		temp_device_altsetting.endpoint[l].bRefresh,
						.bSynchAddress = 	temp_device_altsetting.endpoint[l].bSynchAddress,
					};
					// When a full-speed device is proxied through a high-speed
					// gadget, convert isochronous bInterval from FS (ms) to HS
					// (125µs microframes): HS interval = 2^(bInterval-1) * 125µs.
					// FS bInterval=1 (1ms) → HS bInterval=4 (1ms).
					if (device_speed == USB_SPEED_FULL &&
					    (temp_endpoint.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK)
					     == USB_ENDPOINT_XFER_ISOC) {
						uint8_t fs_interval = temp_endpoint.bInterval;
						// Convert ms to nearest 125µs exponent:
						// fs_interval ms = fs_interval * 8 microframes
						// 2^(n-1) = fs_interval * 8 → n = log2(fs_interval*8) + 1
						// For bInterval=1: n = log2(8)+1 = 4
						uint8_t hs_interval = 4;
						if (fs_interval > 1) {
							int val = fs_interval * 8;
							hs_interval = 1;
							while (val > 1) {
								val >>= 1;
								hs_interval++;
							}
						}
						if (hs_interval > 16)
							hs_interval = 16;
						printf("Converting ISO bInterval %d (FS ms) -> %d (HS 125us)\n",
							fs_interval, hs_interval);
						temp_endpoint.bInterval = hs_interval;
					}

					temp_endpoints[l].endpoint = temp_endpoint;
					temp_endpoints[l].device_bEndpointAddress = temp_endpoint.bEndpointAddress;
					temp_endpoints[l].udc_maxpacket_limit = 0;
					temp_endpoints[l].thread_read = 0;
					temp_endpoints[l].thread_write = 0;
					memset((void *)&temp_endpoints[l].thread_info, 0,
						sizeof(temp_endpoints[l].thread_info));
					temp_endpoints[l].thread_info.ep_num = -1;
				}
				temp_altsettings[k].endpoints = temp_endpoints;
			}
			temp_interfaces[j].altsettings = temp_altsettings;
			temp_interfaces[j].num_altsettings = device_config_desc[i]->interface[j].num_altsetting;
			temp_interfaces[j].current_altsetting = 0;

		}
		host_device_desc.configs[i].interfaces = temp_interfaces;
	}

	host_device_desc.current_config = 0;

	return 0;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	// The default 50us timer slack inflates every short sleep/bounded wait on
	// the forwarding paths, and those waits sit on the per-transfer round trip.
	prctl(PR_SET_TIMERSLACK, 1000UL);

	const char *device = "dummy_udc.0";
	const char *driver = "dummy_udc";
	int vendor_id = -1;
	int product_id = -1;

	struct sigaction action;
	memset(&action, 0, sizeof(struct sigaction));
	action.sa_handler = handle_signal;
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGINT, &action, NULL);
	// SIGUSR1 interrupts blocking raw-gadget ioctls in the endpoint, console
	// and idle-gadget threads. Installed here, before any of them exists,
	// so a signal sent to an early thread cannot hit the default action.
	signal(SIGUSR1, noop_signal_handler);

	int opt, lopt, loidx;
	const char *optstring = "hv";
	const struct option long_options[] = {
		{"help", no_argument, &lopt, 1},
		{"verbose", no_argument, &lopt, 2},
		{"device", required_argument, &lopt, 3},
		{"driver", required_argument, &lopt, 4},
		{"vendor_id", required_argument, &lopt, 5},
		{"product_id", required_argument, &lopt, 6},
		{"enable_injection", no_argument, &lopt, 7},
		{"injection_file", required_argument, &lopt, 8},
		{"enable_customized_config", no_argument, &lopt, 9},
		{"auto_remap_endpoints", no_argument, &lopt, 10},
		{"iso_batch_size", required_argument, &lopt, 11},
		{"bulk_out_in_flight", required_argument, &lopt, 12},
		{"adb_bulk_diag", no_argument, &lopt, 13},
		{"musb_out_read_packets", required_argument, &lopt, 14},
		{"adb_ack_accel", no_argument, &lopt, 15},
		{"power_hook", required_argument, &lopt, 16},
		{"power_idle_ms", required_argument, &lopt, 17},
		{"settle_ms", required_argument, &lopt, 18},
		{"usb_console", no_argument, &lopt, 19},
		{"usb_console_idle", no_argument, &lopt, 20},
		{"usb_console_idle_delay_ms", required_argument, &lopt, 21},
		{"usb_console_shell", required_argument, &lopt, 22},
		{"min_off_ms", required_argument, &lopt, 23},
		{"persistent_gadget", no_argument, &lopt, 24},
		{"gadget_vendor_id", required_argument, &lopt, 25},
		{"gadget_product_id", required_argument, &lopt, 26},
		{"gadget_serial", required_argument, &lopt, 27},
		{0, 0, 0, 0}
	};
	while ((opt = getopt_long(argc, argv, optstring, long_options, &loidx)) != -1) {
		if(opt == 0)
			opt = lopt;
		switch (opt) {
		case 'h':
			usage();
			break;
		case 'v':
			verbose_level++;
			break;
		case 1:
			usage();
			break;
		case 2:
			verbose_level++;
			break;
		case 3:
			device = optarg;
			break;
		case 4:
			driver = optarg;
			break;
		case 5:
			vendor_id = std::stoul(optarg, nullptr, 16);
			break;
		case 6:
			product_id = std::stoul(optarg, nullptr, 16);
			break;
		case 7:
			injection_enabled = true;
			break;
		case 8:
			injection_file = optarg;
			injection_enabled = true;
			break;
		case 9:
			customized_config_enabled = true;
			break;
		case 10:
			auto_remap_endpoints = true;
			printf("Automatic endpoint remapping enabled\n");
			break;
		case 11:
			iso_batch_size = std::stoi(optarg);
			if (iso_batch_size < 1)
				iso_batch_size = 1;
			if (iso_batch_size > ISO_BATCH_SIZE_MAX)
				iso_batch_size = ISO_BATCH_SIZE_MAX;
			printf("Isochronous batch size set to %d\n", iso_batch_size);
			break;
		case 12:
			bulk_out_max_in_flight = std::stoi(optarg);
			if (bulk_out_max_in_flight < 0)
				bulk_out_max_in_flight = 0;
			if (bulk_out_max_in_flight > BULK_OUT_IN_FLIGHT_MAX)
				bulk_out_max_in_flight = BULK_OUT_IN_FLIGHT_MAX;
			printf("Bulk-OUT in-flight set to %d%s\n", bulk_out_max_in_flight,
				bulk_out_max_in_flight == 0 ? " (synchronous)" : "");
			break;
		case 13:
			adb_bulk_diag = true;
			printf("ADB bulk diagnostic enabled\n");
			break;
		case 14:
			musb_out_read_packets = std::stoi(optarg);
			if (musb_out_read_packets < 1)
				musb_out_read_packets = 1;
			if (musb_out_read_packets > 64)
				musb_out_read_packets = 64;
			printf("musb bulk-OUT read packets set to %d\n", musb_out_read_packets);
			break;
		case 15:
			adb_ack_accel = true;
			printf("ADB ACK accelerator enabled\n");
			break;
		case 16:
			power_hook = optarg;
			break;
		case 17:
			power_idle_ms = std::stoi(optarg);
			if (power_idle_ms < POWER_IDLE_MS_MIN)
				power_idle_ms = POWER_IDLE_MS_MIN;
			break;
		case 18:
			device_settle_ms = std::stoi(optarg);
			if (device_settle_ms < 0)
				device_settle_ms = 0;
			if (device_settle_ms > 0)
				printf("Device settle set to %d ms\n", device_settle_ms);
			break;
		case 19:
			usb_console = true;
			break;
		case 20:
			usb_console_idle = true;
			break;
		case 21:
			usb_console_idle_delay_ms = std::stoi(optarg);
			if (usb_console_idle_delay_ms < 0)
				usb_console_idle_delay_ms = 0;
			break;
		case 22:
			usb_console_shell = optarg;
			break;
		case 23:
			usb_console_min_off_ms = std::stoi(optarg);
			if (usb_console_min_off_ms < 0)
				usb_console_min_off_ms = 0;
			break;
		case 24:
			persistent_gadget = true;
			break;
		case 25:
			gadget_vendor_id = std::stoul(optarg, nullptr, 16) & 0xffff;
			break;
		case 26:
			gadget_product_id = std::stoul(optarg, nullptr, 16) & 0xffff;
			break;
		case 27:
			gadget_serial = optarg;
			break;

		default:
			usage();
			return 1;
		}
	}
	// The musb-hdrc gadget controller mishandles OUT requests whose buffer is
	// larger than one packet, so OUT reads are clamped to wMaxPacketSize for it.
	gadget_is_musb = (strstr(driver, "musb") != NULL);
	gadget_driver = driver;
	gadget_device = device;
	printf("Device is: %s\n", device);
	printf("Driver is: %s\n", driver);
	printf("vendor_id is: %d\n", vendor_id);
	printf("product_id is: %d\n", product_id);

	if (injection_enabled) {
		printf("Injection enabled\n");
		if (injection_file.empty()) {
			printf("Injection file not specified\n");
			return 1;
		}
		struct stat buffer;
		if (stat(injection_file.c_str(), &buffer) != 0) {
			printf("Injection file %s not found\n", injection_file.c_str());
			return 1;
		}

		Json::Reader jsonReader;
		std::ifstream ifs(injection_file.c_str());
		if (jsonReader.parse(ifs, injection_config))
			printf("Parsed injection file: %s\n", injection_file.c_str());
		else {
			printf("Error parsing injection file: %s\n", injection_file.c_str());
			return 1;
		}
		ifs.close();
		print_injection_summary();
	}

	if (customized_config_enabled) {
		struct stat buffer;
		if (stat(customized_config_file.c_str(), &buffer) != 0) {
			printf("Customized config file %s not found\n", customized_config_file.c_str());
			return 1;
		}

		Json::Reader jsonReader;
		std::ifstream ifs(customized_config_file.c_str());
		Json::Value customized_config;
		if (jsonReader.parse(ifs, customized_config))
			printf("Parsed customized config file: %s\n", customized_config_file.c_str());
		else {
			printf("Error parsing customized config file: %s\n", customized_config_file.c_str());
			return 1;
		}
		ifs.close();

		if (customized_config["reset_device_before_proxy"] == false) {
			printf("reset_device_before_proxy set to false\n");
			reset_device_before_proxy = false;
		}
		if (customized_config["bmaxpacketsize0_must_greater_than_64"] == false) {
			printf("bmaxpacketsize0_must_greater_than_64 set to false\n");
			bmaxpacketsize0_must_greater_than_64 = false;
		}
		if (customized_config.get("drop_zero_len_out", false).asBool()) {
			printf("drop_zero_len_out enabled (host OUT ZLPs not forwarded)\n");
			drop_zero_len_out = true;
		}
		if (customized_config.isMember("async_bulk_out_in_flight")) {
			int v = customized_config["async_bulk_out_in_flight"].asInt();
			if (v < 0)
				v = 0;
			if (v > BULK_OUT_IN_FLIGHT_MAX)
				v = BULK_OUT_IN_FLIGHT_MAX;
			bulk_out_max_in_flight = v;
			printf("async_bulk_out_in_flight set to %d%s\n", v,
				v == 0 ? " (synchronous)" : "");
		}
		if (customized_config.get("adb_bulk_diag", false).asBool()) {
			printf("adb_bulk_diag enabled (ADB/file-sync bulk OUT logging)\n");
			adb_bulk_diag = true;
		}
		if (customized_config.isMember("musb_out_read_packets")) {
			int v = customized_config["musb_out_read_packets"].asInt();
			if (v < 1)
				v = 1;
			if (v > 64)
				v = 64;
			musb_out_read_packets = v;
			printf("musb_out_read_packets set to %d\n", v);
		}
		if (customized_config.get("adb_ack_accel", false).asBool()) {
			printf("adb_ack_accel enabled (local WRTE acks both directions; real OKAYs swallowed)\n");
			adb_ack_accel = true;
		}
		if (customized_config.isMember("power_hook"))
			power_hook = customized_config["power_hook"].asString();
		if (customized_config.isMember("power_idle_ms")) {
			int v = customized_config["power_idle_ms"].asInt();
			power_idle_ms = v < POWER_IDLE_MS_MIN ? POWER_IDLE_MS_MIN : v;
		}
		if (customized_config.get("usb_console", false).asBool())
			usb_console = true;
		if (customized_config.get("usb_console_idle", false).asBool())
			usb_console_idle = true;
		if (customized_config.isMember("usb_console_idle_delay_ms")) {
			int v = customized_config["usb_console_idle_delay_ms"].asInt();
			usb_console_idle_delay_ms = v < 0 ? 0 : v;
		}
		if (customized_config.isMember("usb_console_shell"))
			usb_console_shell = customized_config["usb_console_shell"].asString();
		if (customized_config.isMember("min_off_ms")) {
			int v = customized_config["min_off_ms"].asInt();
			usb_console_min_off_ms = v < 0 ? 0 : v;
		}
		if (customized_config.get("persistent_gadget", false).asBool())
			persistent_gadget = true;
		if (customized_config.isMember("gadget_vendor_id"))
			gadget_vendor_id = std::stoul(customized_config["gadget_vendor_id"].asString(),
						      nullptr, 16) & 0xffff;
		if (customized_config.isMember("gadget_product_id"))
			gadget_product_id = std::stoul(customized_config["gadget_product_id"].asString(),
						       nullptr, 16) & 0xffff;
		if (customized_config.isMember("gadget_serial"))
			gadget_serial = customized_config["gadget_serial"].asString();
	}
	if (persistent_gadget) {
		// The fixed gadget carries the console itself and is always on the
		// bus, so the idle gadget has no role; and its endpoints come from
		// the UDC's own pool, so remapping is inherent.
		if (!auto_remap_endpoints)
			printf("persistent_gadget implies --auto_remap_endpoints\n");
		auto_remap_endpoints = true;
		if (usb_console_idle)
			printf("persistent_gadget: usb_console_idle ignored (the fixed gadget is always attached)\n");
		usb_console_idle = false;
		usb_console = true;
		printf("persistent_gadget enabled: fixed gadget %04x:%04x serial %s; the host sees this\n"
		       "identity, not the proxied device's, and is never re-enumerated on device changes\n",
		       gadget_vendor_id, gadget_product_id, gadget_serial.c_str());
	}
	if (usb_console)
		printf("usb_console enabled (CDC-ACM shell `%s`%s, idle delay %d ms, min off %d ms)\n",
			usb_console_shell.c_str(),
			usb_console_idle ? ", idle console" : "",
			usb_console_idle_delay_ms, usb_console_min_off_ms);

	// After option and config parsing, so both sources are honoured.
	power_policy_start();

	// The console shell exists for the life of the process, so the idle
	// console has a prompt the moment it enumerates.
	if (usb_console)
		shell_start(usb_console_shell);

	// Persistent gadget: attach the fixed gadget now and run the device
	// manager. Devices that fit the adb/fastboot template are bridged onto
	// it, for as long as they last, without the host ever seeing a change;
	// anything else is ignored while it is on the bus. Only when the fixed
	// gadget cannot attach at all do we drop through to the transparent path.
	if (persistent_gadget) {
		bridge_setup_desc();
		if (!fixed_gadget_start(driver, device)) {
			printf("persistent_gadget: cannot attach the fixed gadget; "
			       "falling back to the transparent mirror\n");
		} else {
			for (;;) {
				while (connect_device(vendor_id, product_id)) {
					if (please_stop_ep0)
						break;
					usleep(200 * 1000);
				}
				if (please_stop_ep0) {
					fixed_gadget_stop();
					shell_stop();
					please_stop_hotplug_monitor = true;
					if (hotplug_monitor_thread)
						pthread_join(hotplug_monitor_thread, NULL);
					return 0;
				}
				printf("[%.3f] Device opened successfully\n", uptime_s());
				power_note_activity(1);
				// Any loss flagged from here on is this device's.
				bridge_clear_device_lost();

				int slot = -1, config_value = 0;
				uint8_t dev_in = 0, dev_out = 0;
				int ifnum = pick_bridge_interface(&slot, &dev_in, &dev_out,
								  &config_value);
				if (ifnum < 0) {
					// Not adb/fastboot: e.g. the mass-storage instance the
					// watch presents while booting. Never rebuild the gadget
					// for it (that would re-enumerate the host and orphan
					// the console for a transient); hold it, ignored, until
					// it leaves the bus, then look again.
					printf("[%.3f] device does not fit the adb/fastboot template "
					       "(no ff/42/01 or ff/42/03 bulk interface); ignoring it "
					       "until it leaves the bus\n", uptime_s());
					bridge_wait_device_lost(-1);
					disconnect_device(-1);
					continue;
				}

				set_configuration(config_value);
				claim_interface(ifnum);
				if (bridge_device_lost_pending()) {
					printf("[%.3f] device vanished while being claimed\n", uptime_s());
					disconnect_device(ifnum);
					continue;
				}
				if (!bridge_bind(slot, dev_in, dev_out)) {
					disconnect_device(ifnum);
					usleep(200 * 1000);
					continue;
				}
				printf("[%.3f] bound interface %d of the device to the %s slot\n",
				       uptime_s(), ifnum, slot == BRIDGE_SLOT_ADB ? "adb" : "fastboot");

				// The host's CNXN, if its transport was restarted, arrives
				// within milliseconds of the bind; give it half a second,
				// then replay the previous one if the transport is stale.
				if (slot == BRIDGE_SLOT_ADB && !bridge_wait_device_lost(500))
					bridge_replay_cnxn_if_needed();

				bridge_wait_device_lost(-1);
				bridge_unbind();
				disconnect_device(ifnum);
				printf("[%.3f] device released; waiting for the next one\n", uptime_s());
			}
		}
	}

	// connect_device() itself waits for a device to appear; a non-zero return
	// is a failed attempt on one that is there (or vanished mid-attempt), so
	// retry quickly rather than adding a second to every enumeration.
	while (connect_device(vendor_id, product_id)) {
		if (please_stop_ep0) {
			idle_gadget_stop();
			shell_stop();
			return 0;
		}
		usleep(200 * 1000);
	}
	printf("[%.3f] Device opened successfully\n", uptime_s());
	// The board may have wound down while waiting; the host's enumeration
	// burst is imminent, so wind up now rather than on its first ep0 event.
	power_note_activity(1);

	// Detect physical device speed.
	int libusb_speed = libusb_get_device_speed(libusb_get_device(dev_handle));
	switch (libusb_speed) {
	case LIBUSB_SPEED_LOW:
		device_speed = USB_SPEED_LOW;
		printf("Device speed: Low Speed (1.5Mbps)\n");
		break;
	case LIBUSB_SPEED_FULL:
		device_speed = USB_SPEED_FULL;
		printf("Device speed: Full Speed (12Mbps)\n");
		break;
	case LIBUSB_SPEED_HIGH:
		device_speed = USB_SPEED_HIGH;
		printf("Device speed: High Speed (480Mbps)\n");
		break;
	case LIBUSB_SPEED_SUPER:
	case LIBUSB_SPEED_SUPER_PLUS:
		device_speed = USB_SPEED_SUPER;
		printf("Device speed: SuperSpeed (5Gbps+)\n");
		break;
	default:
		device_speed = USB_SPEED_HIGH;
		printf("Device speed: Unknown, defaulting to High Speed\n");
		break;
	}

	setup_host_usb_desc();
	printf("Setup USB config successfully\n");

	// If the idle console was on the bus a moment ago, give the host time to
	// see the disconnect before a different gadget appears on the same port.
	idle_gadget_wait_min_off(usb_console_min_off_ms);

	int fd = usb_raw_open();
	// Always use USB_SPEED_HIGH for the gadget; some UDCs (e.g., musb-hdrc)
	// reject lower speeds. We compensate by adjusting bInterval below.
	// The UDC may still be unbinding from the idle gadget: retry briefly on
	// EBUSY before giving up (the exiting wrapper would take the process down
	// and inittab would respawn it, which also works, only slower).
	int rv_init = usb_raw_init_try(fd, USB_SPEED_HIGH, driver, device);
	if (rv_init < 0) {
		errno = -rv_init;
		perror("ioctl(USB_RAW_IOCTL_INIT)");
		exit(EXIT_FAILURE);
	}
	int rv_run = -EBUSY;
	for (int attempt = 0; attempt < 40 && rv_run == -EBUSY; attempt++) {
		rv_run = usb_raw_run_try(fd);
		if (rv_run == -EBUSY)
			usleep(50 * 1000);
	}
	if (rv_run < 0) {
		errno = -rv_run;
		perror("ioctl(USB_RAW_IOCTL_RUN)");
		exit(EXIT_FAILURE);
	}

	if (remap_host_endpoints_if_needed(fd) < 0) {
		close(fd);
		return 1;
	}

	ep0_loop(fd);

	// ep0_loop returned without _exit()ing (a shutdown signal, not a device
	// disconnect). Tell hotplug_monitor to stop so the join below completes;
	// otherwise it loops forever and main() wedges here with the gadget half-up.
	please_stop_hotplug_monitor = true;

	close(fd);
	shell_stop();

	int bNumConfigurations = device_device_desc.bNumConfigurations;
	for (int i = 0; i < bNumConfigurations; i++) {
		int bNumInterfaces = device_config_desc[i]->bNumInterfaces;
		for (int j = 0; j < bNumInterfaces; j++) {
			int num_altsetting = device_config_desc[i]->interface[j].num_altsetting;
			for (int k = 0; k < num_altsetting; k++) {
				if (host_device_desc.configs[i].interfaces[j].altsettings[k].endpoints) {
					delete[] host_device_desc.configs[i].interfaces[j].altsettings[k].endpoints;
				}
			}
			delete[] host_device_desc.configs[i].interfaces[j].altsettings;
		}
		delete[] host_device_desc.configs[i].interfaces;
	}
	delete[] host_device_desc.configs;
	delete[] device_config_desc;

	if (context && callback_handle != -1) {
		libusb_hotplug_deregister_callback(context, callback_handle);
	}
	if (hotplug_monitor_thread &&
		pthread_join(hotplug_monitor_thread, NULL)) {
		fprintf(stderr, "Error join hotplug_monitor_thread\n");
	}

	return 0;
}

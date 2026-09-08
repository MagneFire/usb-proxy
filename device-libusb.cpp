#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include <errno.h>
#include <unistd.h>

#include "device-libusb.h"
#include "proxy.h"

libusb_device 			**devs;
libusb_device_handle 		*dev_handle;
libusb_context 			*context = NULL;
libusb_hotplug_callback_handle	callback_handle = -1;

struct libusb_device_descriptor		device_device_desc;
struct libusb_config_descriptor		**device_config_desc;

pthread_t hotplug_monitor_thread;

// devtmpfs node of the opened device (/dev/bus/usb/BBB/DDD). The kernel removes
// it the moment the device leaves the bus, and a device that comes back gets a
// fresh address, so its absence is an unambiguous, bus-traffic-free "gone".
static char device_node_path[32];

int hotplug_callback(struct libusb_context *ctx __attribute__((unused)),
			struct libusb_device *dev __attribute__((unused)),
			libusb_hotplug_event envet __attribute__((unused)),
			void *user_data __attribute__((unused))) {
	// Used to kill(0, SIGINT), which only sets the stop flags: the ep0 thread
	// blocked in USB_RAW_IOCTL_EVENT_FETCH is not interrupted unless the signal
	// happens to land on it, and even then main() ends in a pthread_join on
	// this never-ending thread. Exit the way the endpoint threads do instead.
	printf("Hotplug event: device disconnected, exiting usb-proxy\n");
	drain_in_queues_and_exit();
	return 0;
}

void *hotplug_monitor(void *arg __attribute__((unused))) {
	printf("Start hotplug_monitor/event thread, thread id(%d)\n", gettid());
	while(true) {
		// This is the SOLE thread that calls libusb_handle_events.
		// All other threads (ISO IN, ISO OUT) submit async transfers
		// and spin-wait on their completion flags.  This avoids event
		// lock contention that would otherwise starve ISO OUT sends.
		struct timeval tv = {1, 0};
		libusb_handle_events_timeout(context, &tv);

		// Liveness check independent of traffic and of hotplug delivery:
		// with the host idle and no endpoint thread running there may be
		// no transfer to fail with NO_DEVICE, and the proxy would otherwise
		// sit on a dead handle with the gadget still attached.
		if (device_node_path[0] && access(device_node_path, F_OK) != 0 &&
		    errno == ENOENT) {
			printf("Device node %s gone, exiting usb-proxy\n", device_node_path);
			drain_in_queues_and_exit();
		}
	}
}

int get_descriptor(libusb_device *device) {
	int result;
	result = libusb_get_device_descriptor(device, &device_device_desc);
	if (result != LIBUSB_SUCCESS) {
		if (verbose_level) {
			fprintf(stderr, "Error retrieving device descriptor: %s\n",
					libusb_strerror((libusb_error)result));
		}
		return result;
	}

	device_config_desc = new struct libusb_config_descriptor *[device_device_desc.bNumConfigurations]();
	for (int i = 0; i < device_device_desc.bNumConfigurations; i++) {
		result = libusb_get_config_descriptor(device, i, &device_config_desc[i]);
		if (result != LIBUSB_SUCCESS) {
			if (verbose_level) {
				fprintf(stderr, "Error retrieving configuration(%d) descriptor: %s\n",
						i, libusb_strerror((libusb_error)result));
			}
			return result;
		}
	}

	return LIBUSB_SUCCESS;
}

int device_settle_ms = 0;

// Undo a partial connect so the caller can simply retry: close the handle and
// drop the config descriptors get_descriptor() allocated for this attempt.
static void drop_device_attempt(void) {
	if (dev_handle) {
		libusb_close(dev_handle);
		dev_handle = NULL;
	}
	if (device_config_desc) {
		for (int i = 0; i < device_device_desc.bNumConfigurations; i++)
			if (device_config_desc[i])
				libusb_free_config_descriptor(device_config_desc[i]);
		delete[] device_config_desc;
		device_config_desc = NULL;
	}
	device_node_path[0] = '\0';
}

int connect_device(int vendor_id, int product_id) {
	int result;
	if (!context) {
		result = libusb_init(&context);
		if (result < 0) {
			fprintf(stderr, "Init error: %s\n", libusb_strerror((libusb_error)result));
			context = NULL;
			return 1;
		}
		libusb_set_debug(context, 3);
	}

	libusb_device *found = NULL;
	bool announced = false;

	// Wait here for a device rather than exiting and being respawned by a
	// polling launcher. Measured on the appliance: a launcher cycle of
	// `sleep 2; exit` plus init's respawn cost ~3 s, so a device that came back
	// waited 0-3 s after the kernel had enumerated it before the proxy even
	// started, and the Mac then saw it ~3.8 s after enumeration versus 1.2 s
	// when the proxy was already waiting. A device-list scan is a sysfs
	// readdir plus a handful of reads (~5 ms at 648 MHz), so a 100 ms poll is
	// free and reacts within a poll period.
	while (found == NULL) {
		int cnt = libusb_get_device_list(context, &devs);
		if (cnt < 0) {
			fprintf(stderr, "Get Device Error: %s\n",
					libusb_strerror((libusb_error)cnt));
			return 1;
		}
		if (verbose_level > 1)
			printf("%d Devices in list\n", cnt);

		for (int i = 0; i < cnt; i++) {
			libusb_device *dvc = devs[i];
			// Device descriptor only (cached by libusb, no allocation):
			// the config descriptors are fetched once, for the device
			// actually chosen, so an idle poll allocates nothing.
			struct libusb_device_descriptor desc;
			if (libusb_get_device_descriptor(dvc, &desc) != LIBUSB_SUCCESS)
				continue;

			if (desc.bDeviceClass == LIBUSB_CLASS_HUB)
				continue;

			if (vendor_id == -1 && product_id == -1) {
				found = dvc;
				break;
			}
			else if ((vendor_id == desc.idVendor || vendor_id == LIBUSB_HOTPLUG_MATCH_ANY) &&
				(product_id == desc.idProduct || product_id == LIBUSB_HOTPLUG_MATCH_ANY)) {
				found = dvc;
				break;
			}
		}

		if (!found) {
			if (!announced) {
				printf("[%.3f] no USB device to proxy yet; polling every %d ms\n",
					uptime_s(), DEVICE_POLL_MS);
				announced = true;
			}
			libusb_free_device_list(devs, 1);
			usleep(DEVICE_POLL_MS * 1000);
		}
	}

	snprintf(device_node_path, sizeof(device_node_path),
		"/dev/bus/usb/%03d/%03d",
		libusb_get_bus_number(found), libusb_get_device_address(found));
	printf("[%.3f] device found: %s%s\n", uptime_s(), device_node_path,
		device_settle_ms > 0 ? " (settling)" : "");

	// Settle debounce (opt-in, --settle_ms). The watch re-enumerates several
	// times on a cradle attach (instances living 2.2 s, 0.33 s, then the real
	// one) and once more around each reboot. Proxying a transient one puts the
	// host through a pointless enumerate/vanish cycle -- the recorded trigger
	// for macOS keeping a stale device object -- so a newly appeared device
	// must survive the settle before the gadget attaches. The devtmpfs node is
	// the liveness test: the kernel removes it the instant the device leaves,
	// and a returning device gets a new address, so no bus traffic is needed.
	if (device_settle_ms > 0) {
		usleep(device_settle_ms * 1000);
		if (access(device_node_path, F_OK) != 0) {
			printf("Device %s vanished during the %d ms settle; waiting for the next one\n",
				device_node_path, device_settle_ms);
			libusb_free_device_list(devs, 1);
			device_node_path[0] = '\0';
			return 1;
		}
	}

	result = get_descriptor(found);
	if (verbose_level)
		printf("[%.3f] descriptors read\n", uptime_s());
	if (result != LIBUSB_SUCCESS) {
		libusb_free_device_list(devs, 1);
		drop_device_attempt();
		return result;
	}

	result = libusb_open(found, &dev_handle);
	libusb_free_device_list(devs, 1);
	if (verbose_level)
		printf("[%.3f] libusb_open done (%d)\n", uptime_s(), result);
	if (result != LIBUSB_SUCCESS) {
		if (verbose_level) {
			fprintf(stderr, "Error opening device handle: %s\n",
					libusb_strerror((libusb_error)result));
		}
		dev_handle = NULL;
		drop_device_attempt();
		return result;
	}

	result = libusb_set_auto_detach_kernel_driver(dev_handle, 0);
	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr, "libusb_set_auto_detach_kernel_driver() failed: %s\n",
				libusb_strerror((libusb_error)result));
		drop_device_attempt();
		return result;
	}

	int config = 0;
	result = libusb_get_configuration(dev_handle, &config);
	if (verbose_level)
		printf("[%.3f] get_configuration done (%d)\n", uptime_s(), result);
	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr, "libusb_get_configuration() failed: %s\n",
				libusb_strerror((libusb_error)result));
		drop_device_attempt();
		return result;
	}

	for (int i = 0; i < device_device_desc.bNumConfigurations; i++) {
		if (device_config_desc[i]->bConfigurationValue != config)
			continue;
		for (int j = 0; j < device_config_desc[i]->bNumInterfaces; j++)
			libusb_detach_kernel_driver(dev_handle, j);
	}

	if (reset_device_before_proxy) {
		result = libusb_reset_device(dev_handle);
		if (result != LIBUSB_SUCCESS) {
			fprintf(stderr, "libusb_reset_device() failed: %s\n",
					libusb_strerror((libusb_error)result));
			drop_device_attempt();
			return result;
		}
	}

	//check that device is responsive
	unsigned char unused[4];
	if (verbose_level)
		printf("[%.3f] kernel drivers detached; probing string descriptor 0\n", uptime_s());
	result = libusb_get_string_descriptor(dev_handle, 0, 0, unused, sizeof(unused));
	if (verbose_level)
		printf("[%.3f] string descriptor probe done (%d)\n", uptime_s(), result);
	if (result < 0) {
		fprintf(stderr, "Device unresponsive: %s\n",
				libusb_strerror((libusb_error)result));
		drop_device_attempt();
		return result;
	}

	if (callback_handle == -1) {
		result = libusb_hotplug_register_callback(context,
			(libusb_hotplug_event) (LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
			(libusb_hotplug_flag) 0, vendor_id, product_id,
			LIBUSB_HOTPLUG_MATCH_ANY, hotplug_callback, NULL, &callback_handle);

		if (result != LIBUSB_SUCCESS) {
			fprintf(stderr, "Error registering callback\n");
			libusb_exit(context);
			return result;
		}
		pthread_create(&hotplug_monitor_thread, 0,
			hotplug_monitor, nullptr);
	}

	return 0;
}

void reset_device() {
	int result = libusb_reset_device(dev_handle);
	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr, "Error resetting device: %s\n",
				libusb_strerror((libusb_error)result));
	}
}

// The configuration/interface helpers run on the ep0 thread before any
// endpoint thread exists, so a device that vanished underneath them has
// nobody else to notice. Same exit as the endpoint threads.
static void device_gone_exit(const char *where) {
	printf("%s: device gone, exiting usb-proxy\n", where);
	drain_in_queues_and_exit();
}

void set_configuration(int configuration) {
	// The proxy host's kernel already configured the device at enumeration,
	// so the host's SET_CONFIGURATION usually asks for the configuration the
	// device is already in. Forwarding that duplicate is not harmless: the
	// legacy Android gadget (e.g. TWRP-era recovery) reacts to a repeat
	// SET_CONFIGURATION by tearing down and re-initialising every function,
	// which kills adbd's endpoints and pulse-disconnects the device off the
	// bus — with the host then re-enumerating, this loops forever. Skipping
	// also keeps both sides' data-toggle state untouched and consistent,
	// whereas forwarding resets only the device's side. On Linux the active
	// configuration is answered from sysfs, so this check puts nothing on
	// the bus.
	int active = -1;
	int result = libusb_get_configuration(dev_handle, &active);
	if (result == LIBUSB_ERROR_NO_DEVICE)
		device_gone_exit("set_configuration");
	if (result == LIBUSB_SUCCESS && active == configuration) {
		printf("Device already in configuration %d, not re-sending SET_CONFIGURATION\n",
				configuration);
		return;
	}

	result = libusb_set_configuration(dev_handle, configuration);
	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr, "Error setting configuration(%d): %s\n",
				configuration, libusb_strerror((libusb_error)result));
		if (result == LIBUSB_ERROR_NO_DEVICE)
			device_gone_exit("set_configuration");
	}
}

void claim_interface(int interface) {
	int result = libusb_claim_interface(dev_handle, interface);
	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr, "Error claiming interface(%d): %s\n",
				interface, libusb_strerror((libusb_error)result));
		if (result == LIBUSB_ERROR_NO_DEVICE)
			device_gone_exit("claim_interface");
	}
}

void release_interface(int interface) {
	int result = libusb_release_interface(dev_handle, interface);
	if (result != LIBUSB_SUCCESS && result != LIBUSB_ERROR_NOT_FOUND) {
		fprintf(stderr, "Error releasing interface(%d): %s\n",
				interface, libusb_strerror((libusb_error)result));
	}
}

void set_interface_alt_setting(int interface, int altsetting) {
	int result = libusb_set_interface_alt_setting(dev_handle, interface, altsetting);
	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr, "Error setting interface altsetting(%d, %d): %s\n",
				interface, altsetting, libusb_strerror((libusb_error)result));
		if (result == LIBUSB_ERROR_NO_DEVICE)
			device_gone_exit("set_interface_alt_setting");
	}
}

int control_request(const usb_ctrlrequest *setup_packet, int *nbytes,
			unsigned char **dataptr, int timeout) {
	int result = libusb_control_transfer(dev_handle,
					setup_packet->bRequestType, setup_packet->bRequest,
					setup_packet->wValue, setup_packet->wIndex, *dataptr,
					setup_packet->wLength, timeout);

	if (result < 0) {
		if (verbose_level) {
			fprintf(stderr, "Error sending setup packet: %s\n",
					libusb_strerror((libusb_error)result));
		}
		if (result == LIBUSB_ERROR_PIPE)
			return -1;
		return result;
	}
	else {
		if (verbose_level)
			printf("Control transfer succeed\n");
	}

	*nbytes = result;
	return 0;
}

int send_iso_data(uint8_t endpoint, uint8_t *dataptr, int length, int timeout);

int send_data(uint8_t endpoint, uint8_t attributes, uint8_t *dataptr,
			int length, int timeout) {
	int transferred;
	int attempt = 0;
	int result = LIBUSB_SUCCESS;

	switch (attributes & USB_ENDPOINT_XFERTYPE_MASK) {
	case USB_ENDPOINT_XFER_CONTROL:
		fprintf(stderr, "Can't send on a control endpoint.\n");
		break;
	case USB_ENDPOINT_XFER_BULK: {
		// Bulk data must never be dropped. The gadget side already ACKed
		// this data to the host, so the host will not resend it; giving up
		// here permanently desyncs a length-framed stream (ADB/fastboot
		// deadlock). A real host controller never times out a NAKing bulk
		// endpoint — it just keeps retrying — so do the same: resend only
		// the unsent tail, indefinitely, until the device accepts it or
		// goes away. Meanwhile the gadget NAKs the host, which is correct
		// end-to-end flow control.
		int off = 0;
		while (true) {
			transferred = 0;
			result = libusb_bulk_transfer(dev_handle, endpoint, dataptr + off,
						length - off, &transferred, timeout);
			off += transferred;
			if (result == LIBUSB_SUCCESS && off >= length)
				break;
			if (result == LIBUSB_SUCCESS || result == LIBUSB_ERROR_TIMEOUT) {
				// Timed out (device NAKing) or a short sync write.
				attempt++;
				if (attempt <= 5 || attempt % 60 == 0)
					fprintf(stderr, "[outdev] EP%02x bulk OUT %s, retrying tail: sent %d/%d, attempt %d\n",
						endpoint,
						result == LIBUSB_ERROR_TIMEOUT ? "timeout" : "short write",
						off, length, attempt);
				if (please_stop_eps)
					break;
				continue;
			}
			// Only a genuine stall (PIPE) is a halt to clear. Clearing on a
			// timeout resets the data toggle, and retrying a timed-out OUT
			// after that reset risks duplicating the transfer.
			if (result == LIBUSB_ERROR_PIPE) {
				libusb_clear_halt(dev_handle, endpoint);
				if (++attempt >= MAX_ATTEMPTS)
					break;
				continue;
			}
			break;	// fatal: NO_DEVICE, IO, ...
		}
		if (result == LIBUSB_SUCCESS) {
			if (attempt)
				fprintf(stderr, "[outdev] EP%02x bulk OUT recovered after %d retries (%d bytes)\n",
					endpoint, attempt, length);
			if (verbose_level > 2)
				printf("Sent %d bytes (Bulk) to EP%02x\n", off, endpoint);
		}
		break;
	}
	case USB_ENDPOINT_XFER_INT:
		result = libusb_interrupt_transfer(dev_handle, endpoint, dataptr, length, &transferred, timeout);

		if (transferred != length)
			fprintf(stderr, "Incomplete Interrupt transfer on EP%02x\n", endpoint);
		if (result == LIBUSB_SUCCESS && verbose_level > 2)
			printf("Sent %d bytes (Int) to libusb EP%02x\n", transferred, endpoint);
		break;
	}
	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr, "Transfer error sending on EP%02x: %s\n",
				endpoint, libusb_strerror((libusb_error)result));
	}
	return result;
}

// ---- Async bulk OUT --------------------------------------------------------
// send_data() above forwards one bulk-OUT packet at a time: every
// libusb_bulk_transfer() blocks for a full USB round trip before the next
// packet is submitted, so the device-side bus goes idle between packets. For a
// sustained download (e.g. a fastboot image) that idle gap throttles
// throughput and lets the gadget-side queue back up until the transfer stalls.
//
// Instead keep up to bulk_out_max_in_flight URBs queued on the endpoint. The
// kernel processes URBs on one endpoint FIFO, so submission order is delivery
// order and the bulk stream stays in sequence. The single event thread
// (hotplug_monitor) drives completions; the callback frees each buffer.
static std::atomic<int> bulk_out_in_flight(0);
static std::atomic<bool> bulk_out_device_gone(false);
static std::atomic<int> bulk_out_error_count(0);

// Diagnostic counters for the -v submit/complete/backpressure trace.
static std::atomic<int> bulk_out_submit_count(0);
static std::atomic<int> bulk_out_complete_count(0);

// Wakes the backpressure wait in send_data_async() as soon as a completion
// frees a slot (instead of a 50us sleep-poll). Bounded wait_for on the waiter
// side keeps a missed notify harmless.
static std::mutex bulk_out_slot_mutex;
static std::condition_variable bulk_out_slot_cv;

static void bulk_out_callback(struct libusb_transfer *transfer) {
	bulk_out_in_flight--;
	bulk_out_slot_cv.notify_one();

	int cc = ++bulk_out_complete_count;
	if (verbose_level > 0)
		fprintf(stderr, "[async] EP%02x complete #%d status=%d actual=%d/%d in_flight=%d\n",
			transfer->endpoint, cc, transfer->status,
			transfer->actual_length, transfer->length, bulk_out_in_flight.load());

	if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
		if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE)
			bulk_out_device_gone = true;
		// NB: do not call libusb_clear_halt() (or any synchronous libusb call)
		// here — this callback runs on the event thread inside
		// libusb_handle_events(), and a blocking control transfer from that
		// context can deadlock. A stalled bulk-OUT download is unrecoverable
		// mid-stream anyway; just report it.

		int n = ++bulk_out_error_count;
		if (n <= 10 || n % 100 == 0)
			fprintf(stderr, "Bulk OUT EP%02x failed: status=%d, %d/%d bytes (total errors: %d)\n",
				transfer->endpoint, transfer->status,
				transfer->actual_length, transfer->length, n);
	} else if (transfer->actual_length != transfer->length) {
		// Short write: the device accepted fewer bytes than we sent. We can't
		// resubmit the tail here without risking reordering against later
		// in-flight URBs on this endpoint, so just report it (a corrupted
		// download fails its own integrity check downstream).
		int n = ++bulk_out_error_count;
		if (n <= 10 || n % 100 == 0)
			fprintf(stderr, "Short bulk OUT on EP%02x: %d/%d bytes (total short: %d)\n",
				transfer->endpoint, transfer->actual_length, transfer->length, n);
	} else if (verbose_level > 2) {
		printf("Sent %d bytes (Bulk async) to EP%02x\n",
			transfer->actual_length, transfer->endpoint);
	}

	// libusb_fill_bulk_transfer() set both ->buffer and ->user_data to the
	// data buffer; free it exactly once.
	delete[] (unsigned char *)transfer->user_data;
	libusb_free_transfer(transfer);
}

int send_data_async(uint8_t endpoint, uint8_t *dataptr, int length, int timeout) {
	if (bulk_out_device_gone) {
		delete[] dataptr;
		return LIBUSB_ERROR_NO_DEVICE;
	}

	int sc = ++bulk_out_submit_count;

	// Backpressure. Bulk data must not be dropped (unlike ISO), so block until
	// an in-flight slot frees rather than discarding the packet. Bail out if
	// the device disappears (a completion callback set the flag) while waiting.
	if (bulk_out_in_flight >= bulk_out_max_in_flight && verbose_level > 0)
		fprintf(stderr, "[async] EP%02x backpressure wait: in_flight=%d (submit #%d)\n",
			endpoint, bulk_out_in_flight.load(), sc);
	while (bulk_out_in_flight >= bulk_out_max_in_flight) {
		if (bulk_out_device_gone) {
			delete[] dataptr;
			return LIBUSB_ERROR_NO_DEVICE;
		}
		std::unique_lock<std::mutex> lock(bulk_out_slot_mutex);
		bulk_out_slot_cv.wait_for(lock, std::chrono::milliseconds(1));
	}

	struct libusb_transfer *transfer = libusb_alloc_transfer(0);
	if (!transfer) {
		fprintf(stderr, "Failed to allocate libusb_transfer for bulk OUT.\n");
		delete[] dataptr;
		return LIBUSB_ERROR_OTHER;
	}

	// The callback frees both the transfer and the buffer (passed via user_data).
	libusb_fill_bulk_transfer(transfer, dev_handle, endpoint, dataptr, length,
				bulk_out_callback, dataptr, timeout);

	int rv = libusb_submit_transfer(transfer);
	if (rv != LIBUSB_SUCCESS) {
		fprintf(stderr, "Bulk OUT submit failed on EP%02x: %s (len=%d)\n",
			endpoint, libusb_strerror((libusb_error)rv), length);
		if (rv == LIBUSB_ERROR_NO_DEVICE)
			bulk_out_device_gone = true;
		libusb_free_transfer(transfer);
		delete[] dataptr;
		return rv;
	}

	bulk_out_in_flight++;
	if (verbose_level > 0)
		fprintf(stderr, "[async] EP%02x submitted #%d len=%d in_flight=%d\n",
			endpoint, sc, length, bulk_out_in_flight.load());
	return LIBUSB_SUCCESS;
}

void iso_transfer_callback(struct libusb_transfer *transfer) {
	int *iso_completed = (int *)transfer->user_data;
	*iso_completed = 1;
}

// Bounded async ISO OUT: submit and return immediately.
// The dedicated event thread (hotplug_monitor) processes completions.
// We limit in-flight transfers to avoid flooding the kernel.
#define ISO_OUT_MAX_IN_FLIGHT 8
static std::atomic<int> iso_out_in_flight(0);

static void iso_out_callback(struct libusb_transfer *transfer) {
	iso_out_in_flight--;
	if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
		static int iso_out_err_count = 0;
		iso_out_err_count++;
		if (iso_out_err_count <= 10 || iso_out_err_count % 100 == 0)
			fprintf(stderr, "ISO OUT EP%02x failed: status=%d (total errors: %d)\n",
				transfer->endpoint, transfer->status, iso_out_err_count);
	}
	// Free the buffer passed via user_data.
	delete[] (unsigned char *)transfer->user_data;
	libusb_free_transfer(transfer);
}

int send_iso_data(uint8_t endpoint, uint8_t *dataptr, int length, int timeout) {
	// If at capacity, wait briefly for a slot.
	int waited_us = 0;
	while (iso_out_in_flight >= ISO_OUT_MAX_IN_FLIGHT && waited_us < 2000) {
		usleep(50);
		waited_us += 50;
	}
	if (iso_out_in_flight >= ISO_OUT_MAX_IN_FLIGHT) {
		// Drop this packet -- ISO is inherently lossy.
		// Free the buffer since the callback won't run.
		delete[] dataptr;
		return LIBUSB_SUCCESS;
	}

	struct libusb_transfer *transfer = libusb_alloc_transfer(1);
	if (!transfer) {
		fprintf(stderr, "Failed to allocate libusb_transfer for ISO OUT.\n");
		return LIBUSB_ERROR_OTHER;
	}

	// The callback frees both the transfer and the buffer (via user_data).
	libusb_fill_iso_transfer(transfer, dev_handle, endpoint, dataptr, length,
				1, iso_out_callback, dataptr, timeout);
	libusb_set_iso_packet_lengths(transfer, length);

	int rv = libusb_submit_transfer(transfer);
	if (rv != LIBUSB_SUCCESS) {
		fprintf(stderr, "ISO OUT submit failed on EP%02x: %s (len=%d)\n",
			endpoint, libusb_strerror((libusb_error)rv), length);
		libusb_free_transfer(transfer);
		return rv;
	}

	iso_out_in_flight++;
	return LIBUSB_SUCCESS;
}

int receive_iso_data_batched(uint8_t endpoint, uint16_t maxPacketSize,
			struct iso_batch_result *result, int batch_size, int timeout) {
	if (batch_size < 1)
		batch_size = 1;
	if (batch_size > ISO_BATCH_SIZE_MAX)
		batch_size = ISO_BATCH_SIZE_MAX;

	memset(result, 0, sizeof(*result));
	result->buffer = new uint8_t[maxPacketSize * batch_size];

	struct libusb_transfer *transfer = libusb_alloc_transfer(batch_size);
	if (!transfer) {
		fprintf(stderr, "Failed to allocate libusb_transfer for ISO batch.\n");
		delete[] result->buffer;
		result->buffer = nullptr;
		return LIBUSB_ERROR_OTHER;
	}

	volatile int iso_completed = 0;
	libusb_fill_iso_transfer(transfer, dev_handle, endpoint, result->buffer,
				maxPacketSize * batch_size, batch_size,
				iso_transfer_callback, (void *)&iso_completed, timeout);
	libusb_set_iso_packet_lengths(transfer, maxPacketSize);

	int rv = libusb_submit_transfer(transfer);
	if (rv != LIBUSB_SUCCESS) {
		if (verbose_level)
			fprintf(stderr, "ISO batch submit failed on EP%02x: %s\n",
				endpoint, libusb_strerror((libusb_error)rv));
		libusb_free_transfer(transfer);
		delete[] result->buffer;
		result->buffer = nullptr;
		return rv;
	}

	// Spin-wait for completion; the dedicated event thread
	// (hotplug_monitor) will call iso_transfer_callback.
	while (!iso_completed)
		usleep(50);

	if (transfer->status != LIBUSB_TRANSFER_COMPLETED &&
	    transfer->status != LIBUSB_TRANSFER_TIMED_OUT) {
		if (verbose_level)
			fprintf(stderr, "ISO batch transfer failed on EP%02x: status %d\n",
				endpoint, transfer->status);
		if (transfer->status == LIBUSB_TRANSFER_STALL)
			libusb_clear_halt(dev_handle, endpoint);
		libusb_free_transfer(transfer);
		delete[] result->buffer;
		result->buffer = nullptr;
		return LIBUSB_ERROR_IO;
	}

	result->num_packets = batch_size;
	uint8_t *packet_ptr = result->buffer;
	for (int i = 0; i < batch_size; i++) {
		result->packets[i].data = packet_ptr;
		result->packets[i].actual_length = transfer->iso_packet_desc[i].actual_length;
		result->packets[i].status = transfer->iso_packet_desc[i].status;
		result->total_length += result->packets[i].actual_length;
		packet_ptr += maxPacketSize;

		if (result->packets[i].status == LIBUSB_TRANSFER_COMPLETED &&
		    result->packets[i].actual_length > 0)
			result->success = true;
	}

	if (verbose_level > 2)
		printf("ISO batch received: %d packets, %d total bytes\n",
			batch_size, result->total_length);

	libusb_free_transfer(transfer);
	return LIBUSB_SUCCESS;
}

int receive_data(uint8_t endpoint, uint8_t attributes, uint16_t maxPacketSize,
			uint8_t **dataptr, int *length, int timeout,
			int read_len) {
	int result = LIBUSB_SUCCESS;

	int attempt = 0;
	int want = maxPacketSize;
	switch (attributes & USB_ENDPOINT_XFERTYPE_MASK) {
	case USB_ENDPOINT_XFER_CONTROL:
		fprintf(stderr, "Can't read on a control endpoint.\n");
		break;
	case USB_ENDPOINT_XFER_ISOC:
		// ISO IN is handled by receive_iso_data_batched() directly.
		fprintf(stderr, "receive_data() should not be called for ISO endpoints.\n");
		break;
	case USB_ENDPOINT_XFER_BULK:
		if (read_len > 0)
			want = read_len;
		*dataptr = new uint8_t[want > maxPacketSize * 8 ? want : maxPacketSize * 8];
		do {
			result = libusb_bulk_transfer(dev_handle, endpoint, *dataptr, want, length, timeout);
			if (result == LIBUSB_SUCCESS && verbose_level > 2)
				printf("Received bulk data(%d) bytes\n", *length);
			// A timeout on a bulk-IN poll is normal (the device simply had
			// no data to send). Only a genuine stall (PIPE) is a halt that
			// warrants clearing; clearing on a timeout resets the data toggle
			// and corrupts an otherwise healthy stream.
			if (result == LIBUSB_ERROR_PIPE)
				libusb_clear_halt(dev_handle, endpoint);

			attempt++;
		} while (result == LIBUSB_ERROR_PIPE && attempt < MAX_ATTEMPTS);
		break;
	case USB_ENDPOINT_XFER_INT:
		*dataptr = new uint8_t[maxPacketSize];
		result = libusb_interrupt_transfer(dev_handle, endpoint, *dataptr, maxPacketSize, length, timeout);
		if (result == LIBUSB_SUCCESS && verbose_level > 2)
			printf("Received int data(%d) bytes\n", *length);
		break;
	}

	// A timeout on a bulk/interrupt IN poll just means the device had nothing
	// to send this round; it is expected and not an error worth reporting.
	if (result != LIBUSB_SUCCESS &&
	    (result != LIBUSB_ERROR_TIMEOUT || verbose_level > 0)) {
		fprintf(stderr, "Transfer error receiving on EP%02x: %s\n",
				endpoint, libusb_strerror((libusb_error)result));
	}

	return result;
}

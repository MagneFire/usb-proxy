#include <vector>
#include <algorithm>
#include <map>
#include <deque>

#include "host-raw-gadget.h"
#include "device-libusb.h"
#include "misc.h"
#include "power-policy.h"

#ifdef HAVE_LUA
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#endif

// UVC Video Streaming interface selectors (USB Video Class spec)
#define UVC_VS_PROBE_CONTROL		0x01
#define UVC_VS_COMMIT_CONTROL		0x02
#define UVC_VS_INPUT_HEADER		0x01
#define UVC_SC_VIDEOSTREAMING		0x02

// Offset of dwMaxPayloadTransferSize in UVC probe/commit response
#define UVC_PROBE_MAX_PAYLOAD_OFFSET	22

extern bool auto_remap_endpoints;

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] |
		((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) |
		((uint32_t)p[3] << 24);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = (v >> 24) & 0xff;
}

static uint32_t adb_cmd(const char s[4])
{
	return (uint32_t)(uint8_t)s[0] |
		((uint32_t)(uint8_t)s[1] << 8) |
		((uint32_t)(uint8_t)s[2] << 16) |
		((uint32_t)(uint8_t)s[3] << 24);
}

static void cmd_to_string(uint32_t cmd, char out[5])
{
	out[0] = cmd & 0xff;
	out[1] = (cmd >> 8) & 0xff;
	out[2] = (cmd >> 16) & 0xff;
	out[3] = (cmd >> 24) & 0xff;
	out[4] = '\0';
}

class AdbSyncDiag {
	enum SyncState {
		SYNC_TAG,
		SYNC_LEN,
		SYNC_DATA,
		SYNC_DONE_VALUE,
	};

	SyncState state = SYNC_TAG;
	std::deque<uint8_t> tag;
	uint8_t lenbuf[4] = {};
	int lenpos = 0;
	uint32_t data_len = 0;
	uint32_t data_seen = 0;
	uint64_t chunk_no = 0;

	void reset_to_tag()
	{
		state = SYNC_TAG;
		tag.clear();
		lenpos = 0;
		data_len = 0;
		data_seen = 0;
	}

	void feed_tag_byte(uint8_t b, uint64_t stream_offset)
	{
		tag.push_back(b);
		if (tag.size() < 4)
			return;
		while (tag.size() > 4)
			tag.pop_front();

		uint8_t t[4] = { tag[0], tag[1], tag[2], tag[3] };
		if (!memcmp(t, "DATA", 4)) {
			state = SYNC_LEN;
			lenpos = 0;
			tag.clear();
			return;
		}
		if (!memcmp(t, "DONE", 4)) {
			state = SYNC_DONE_VALUE;
			lenpos = 0;
			tag.clear();
			fprintf(stderr, "[adbdiag] sync DONE tag at stream=%llu\n",
				(unsigned long long)(stream_offset - 3));
			return;
		}
		if (!memcmp(t, "OKAY", 4) || !memcmp(t, "FAIL", 4)) {
			char name[5] = { (char)t[0], (char)t[1], (char)t[2], (char)t[3], '\0' };
			fprintf(stderr, "[adbdiag] sync %s tag at stream=%llu\n",
				name, (unsigned long long)(stream_offset - 3));
			tag.clear();
			return;
		}

		tag.pop_front();
	}

public:
	void gap(const char *reason, uint64_t stream_offset)
	{
		if (state == SYNC_DATA) {
			fprintf(stderr,
				"[adbdiag] DATA #%llu interrupted by %s at stream=%llu expected=%u observed=%u remaining=%u\n",
				(unsigned long long)chunk_no, reason,
				(unsigned long long)stream_offset, data_len, data_seen,
				data_len - data_seen);
		} else if (state == SYNC_LEN) {
			fprintf(stderr,
				"[adbdiag] DATA #%llu length interrupted by %s at stream=%llu bytes_read=%d/4\n",
				(unsigned long long)(chunk_no + 1), reason,
				(unsigned long long)stream_offset, lenpos);
		}
	}

	void feed(uint8_t b, uint64_t stream_offset)
	{
		switch (state) {
		case SYNC_TAG:
			feed_tag_byte(b, stream_offset);
			break;
		case SYNC_LEN:
			lenbuf[lenpos++] = b;
			if (lenpos == 4) {
				data_len = le32(lenbuf);
				data_seen = 0;
				chunk_no++;
				fprintf(stderr, "[adbdiag] DATA #%llu expected=%u stream=%llu\n",
					(unsigned long long)chunk_no, data_len,
					(unsigned long long)(stream_offset - 7));
				if (data_len > 1024 * 1024) {
					fprintf(stderr, "[adbdiag] DATA #%llu invalid length=%u, resyncing\n",
						(unsigned long long)chunk_no, data_len);
					reset_to_tag();
				} else if (data_len == 0) {
					fprintf(stderr, "[adbdiag] DATA #%llu complete observed=0\n",
						(unsigned long long)chunk_no);
					reset_to_tag();
				} else {
					state = SYNC_DATA;
				}
			}
			break;
		case SYNC_DATA:
			if (b != 0xaa) {
				uint32_t remaining = data_len - data_seen;
				fprintf(stderr,
					"[adbdiag] DATA #%llu short/non-aa at stream=%llu expected=%u observed=%u remaining=%u byte=0x%02x\n",
					(unsigned long long)chunk_no,
					(unsigned long long)stream_offset,
					data_len, data_seen, remaining, b);
				reset_to_tag();
				feed_tag_byte(b, stream_offset);
				break;
			}
			data_seen++;
			if (data_seen == data_len) {
				fprintf(stderr, "[adbdiag] DATA #%llu complete observed=%u\n",
					(unsigned long long)chunk_no, data_seen);
				reset_to_tag();
			}
			break;
		case SYNC_DONE_VALUE:
			lenbuf[lenpos++] = b;
			if (lenpos == 4) {
				fprintf(stderr, "[adbdiag] sync DONE mtime=%u\n", le32(lenbuf));
				reset_to_tag();
			}
			break;
		}
	}
};

class AdbBulkDiag {
	std::deque<uint8_t> header;
	uint32_t payload_remaining = 0;
	uint32_t current_cmd = 0;
	uint64_t stream_offset = 0;
	uint64_t msg_no = 0;
	AdbSyncDiag sync;

	bool parse_header(uint8_t out[24], uint32_t *cmd, uint32_t *length)
	{
		for (int i = 0; i < 24; i++)
			out[i] = header[i];

		*cmd = le32(out);
		*length = le32(out + 12);
		uint32_t magic = le32(out + 20);
		if (magic != (*cmd ^ 0xffffffffU))
			return false;
		if (*length > 1024 * 1024)
			return false;
		return true;
	}

public:
	void zlp(uint8_t endpoint)
	{
		static const uint32_t WRTE = adb_cmd("WRTE");

		if (!payload_remaining)
			return;

		char name[5];
		cmd_to_string(current_cmd, name);
		fprintf(stderr,
			"[adbdiag] EP%02x ZLP while ADB %s #%llu payload incomplete: remaining=%u stream=%llu\n",
			endpoint, name, (unsigned long long)msg_no,
			payload_remaining, (unsigned long long)stream_offset);
		if (current_cmd == WRTE)
			sync.gap("host ZLP before ADB payload completion", stream_offset);
	}

	void feed(uint8_t endpoint, const uint8_t *data, int length)
	{
		static const uint32_t WRTE = adb_cmd("WRTE");

		for (int i = 0; i < length; i++, stream_offset++) {
			uint8_t b = data[i];

			if (payload_remaining > 0) {
				if (current_cmd == WRTE)
					sync.feed(b, stream_offset);
				payload_remaining--;
				if (payload_remaining == 0) {
					char name[5];
					cmd_to_string(current_cmd, name);
					fprintf(stderr, "[adbdiag] ADB %s #%llu payload end stream=%llu\n",
						name, (unsigned long long)msg_no,
						(unsigned long long)stream_offset);
				}
				continue;
			}

			header.push_back(b);
			if (header.size() < 24)
				continue;
			while (header.size() > 24)
				header.pop_front();

			uint8_t hdr[24];
			uint32_t cmd;
			uint32_t payload_len;
			if (!parse_header(hdr, &cmd, &payload_len)) {
				header.pop_front();
				continue;
			}

			current_cmd = cmd;
			payload_remaining = payload_len;
			msg_no++;
			char name[5];
			cmd_to_string(cmd, name);
			fprintf(stderr, "[adbdiag] EP%02x ADB %s #%llu len=%u stream=%llu\n",
				endpoint, name, (unsigned long long)msg_no, payload_len,
				(unsigned long long)(stream_offset - 23));
			header.clear();
		}
	}
};

// Separate instances per direction: the host->device (OUT) and device->host
// (IN) bulk streams are independent ADB transports and must not share parser
// state. The EP address in each log line (0x0x=OUT, 0x8x=IN) tells them apart.
static AdbBulkDiag adb_bulk_diag_state;
static AdbBulkDiag adb_bulk_diag_in_state;

// ---- ADB ACK accelerator (--adb_ack_accel) ---------------------------------
//
// A legacy ADB transport (as negotiated by old adbd, e.g. Wear OS 9 watches)
// allows one outstanding WRTE per stream: the host won't send the next 4KB
// payload until the device's A_OKAY comes back. Push throughput is therefore
// 4096 / round-trip, and the proxy's store-and-forward hop roughly triples
// the round trip (~0.45ms direct -> ~1.3ms), i.e. ~9 MB/s drops to ~3 MB/s
// regardless of datapath bandwidth. ADB delayed-ack would fix this properly
// but needs a much newer adbd than such devices have.
//
// The accelerator restores the direct-connection round trip by acking WRTEs
// locally: as soon as a host WRTE's header has been read from the gadget (the
// payload is committed and follows on the same pipe), a fabricated A_OKAY is
// queued toward the host, and the device's real A_OKAYs for that stream pair
// are swallowed. The device's acks can NOT be count-matched one-per-WRTE:
// old adbd defers READY while a stream's buffer is backed up and acks the
// whole backlog with a single READY (see PairState below), so bookkeeping is
// credit-based, not FIFO-matched.
//
// Why this is safe(ish):
// - Transport OKAYs are pure flow control. The file-sync protocol's own
//   DONE -> OKAY/FAIL handshake rides in a device->host WRTE and passes
//   through untouched, so "adb push" success/failure still reflects what
//   adbd actually received and wrote. The window where the host's view can
//   run ahead of the device's acks is capped (ACK_ACCEL_MAX_AHEAD spoofs
//   since the device's last real OKAY); past that the spoof is withheld and
//   the host waits on real flow control.
// - Device-side backpressure is unaffected: bounded async in-flight URBs,
//   bounded gadget-side queue, musb NAKs the host beyond that.
// - Fabricated OKAYs are only inserted at device->host message boundaries,
//   so they can never split another message's header/payload sequence.
// - Fail-open: any framing surprise (unparseable header, or an OKAY that
//   doesn't arrive as a standalone 24-byte read while swallows are owed)
//   permanently disables all meddling for the session and everything is
//   forwarded verbatim; a CNXN (new ADB session) clears stream bookkeeping.
//
// Assumes a single proxied bulk IN/OUT pair speaks ADB (true for these
// watches). A second bulk interface feeding non-ADB data would trip the
// framing check and fail open — correct, just without the speedup.
//
// The same trick runs in the pull direction (device -> host WRTEs, i.e.
// "adb pull"): on a device WRTE header a fabricated OKAY is submitted toward
// the device (send_data_async, so it needs the async bulk-OUT path), and the
// host's real OKAYs for that pair are swallowed on the OUT stream. Same
// credit model, separate per-direction bookkeeping (pull_pairs). The
// device-bound spoof may only be submitted while the host->device parser is
// at a message boundary; since out_feed() runs before the OUT thread submits
// the read it just parsed, the worst interleaving is two *complete* messages
// swapping order, which ADB doesn't care about. Mid-message the spoof parks
// in pending_out and the OUT read thread flushes it right after its own
// submit (out_flush()).
#define ACK_ACCEL_MAX_AHEAD 8

class AdbAckAccel {
	struct Parser {
		uint8_t hdr[24];
		unsigned int hdr_have = 0;
		uint32_t payload_remaining = 0;
		uint32_t cmd = 0, arg0 = 0, arg1 = 0;

		bool at_boundary() const { return hdr_have == 0 && payload_remaining == 0; }
		void reset() { hdr_have = 0; payload_remaining = 0; }
	};

	std::mutex mtx;
	bool dead = false;
	Parser out;	// host -> device stream
	Parser in;	// device -> host stream
	// Per stream pair (host id, device id) we have spoofed on. The device's
	// real OKAYs can NOT be count-matched to WRTEs: old adbd defers its READY
	// while a stream's buffer is backed up (e.g. the transport thread pauses
	// to spawn a shell service) and then acknowledges the whole backlog with
	// a single READY once it drains — acks are an edge-triggered "I have room
	// again" signal, not one-per-WRTE (observed on hardware: 9 WRTEs in,
	// 1 OKAY back). So per pair we keep a spoof-credit counter: spoof freely
	// while credits last; at zero, hold the spoof (the host just waits — real
	// flow control takes over); ANY real OKAY for the pair means the device
	// drained its buffer, so swallow it, refill the credits and release held
	// spoofs.
	struct PairState {
		int tokens = ACK_ACCEL_MAX_AHEAD;
		std::deque<usb_raw_transfer_io> held;
	};
	std::map<std::pair<uint32_t, uint32_t>, PairState> pairs;
	// Fabricated OKAYs waiting for the device->host stream to reach a
	// message boundary before they may be inserted.
	std::deque<usb_raw_transfer_io> pending;
	uint64_t spoofed = 0, swallowed = 0;

	// Pull direction: device-bound spoofs are heap buffers handed to
	// send_data_async (which takes ownership); these deques hold buffers we
	// still own. Keyed (device id, host id) as seen on the device's WRTE.
	struct PullPairState {
		int tokens = ACK_ACCEL_MAX_AHEAD;
		std::deque<uint8_t *> held;
	};
	std::map<std::pair<uint32_t, uint32_t>, PullPairState> pull_pairs;
	// Device-bound spoofs waiting for the host->device stream to reach a
	// message boundary.
	std::deque<uint8_t *> pending_out;
	uint64_t pull_spoofed = 0, pull_swallowed = 0;

	// Lock order everywhere: mtx, then the endpoint queue's data_mutex.

	void die(const char *why)
	{
		if (!dead)
			fprintf(stderr, "[ackaccel] DISABLED for this session: %s "
				"(forwarding verbatim; spoofed=%llu swallowed=%llu "
				"pull_spoofed=%llu pull_swallowed=%llu)\n",
				why, (unsigned long long)spoofed,
				(unsigned long long)swallowed,
				(unsigned long long)pull_spoofed,
				(unsigned long long)pull_swallowed);
		dead = true;
		clear_streams();
	}

	void clear_streams()
	{
		pairs.clear();
		pending.clear();
		for (auto &kv : pull_pairs)
			for (uint8_t *buf : kv.second.held)
				delete[] buf;
		pull_pairs.clear();
		for (uint8_t *buf : pending_out)
			delete[] buf;
		pending_out.clear();
	}

	static bool parse_hdr(const uint8_t *p, uint32_t *cmd, uint32_t *arg0,
			      uint32_t *arg1, uint32_t *len)
	{
		*cmd = le32(p);
		*arg0 = le32(p + 4);
		*arg1 = le32(p + 8);
		*len = le32(p + 12);
		uint32_t magic = le32(p + 20);
		return magic == (*cmd ^ 0xffffffffU) && *len <= 1024 * 1024;
	}

	void enqueue_to(struct thread_info *ti, const usb_raw_transfer_io &io)
	{
		ti->data_mutex->lock();
		ti->data_queue->push_back(io);
		ti->data_mutex->unlock();
		ti->data_cv->notify_all();
	}

	void flush_pending(struct thread_info *in_ti)
	{
		if (verbose_level > 0 && !pending.empty())
			fprintf(stderr, "[ackaccel] flush %zu deferred spoof(s)\n",
				pending.size());
		while (!pending.empty()) {
			enqueue_to(in_ti, pending.front());
			pending.pop_front();
		}
	}

	void on_wrte_complete(struct thread_info *out_ti)
	{
		static const uint32_t OKAY = adb_cmd("OKAY");
		auto key = std::make_pair(out.arg0, out.arg1);
		PairState &st = pairs[key];

		struct usb_raw_transfer_io io;
		memset(&io.inner, 0, sizeof(io.inner));
		io.inner.ep = out_ti->peer_in->ep_num;
		io.inner.flags = 0;
		io.inner.length = 24;
		uint8_t *p = (uint8_t *)io.data;
		put_le32(p, OKAY);
		put_le32(p + 4, out.arg1);	/* device-side stream id */
		put_le32(p + 8, out.arg0);	/* host-side stream id */
		put_le32(p + 12, 0);
		put_le32(p + 16, 0);
		put_le32(p + 20, OKAY ^ 0xffffffffU);

		if (st.tokens <= 0) {
			// Spoof credit exhausted: the device hasn't signalled room
			// since ACK_ACCEL_MAX_AHEAD spoofs. Hold the ack; the host
			// stalls exactly as without the accelerator until the
			// device's next real OKAY releases it. (Window 1 means at
			// most one WRTE arrives after we withhold, so this stays
			// short.)
			fprintf(stderr, "[ackaccel] hold: (%u,%u) out of spoof credit, awaiting device OKAY\n",
				out.arg0, out.arg1);
			st.held.push_back(io);
			return;
		}
		st.tokens--;
		spoofed++;

		if (verbose_level > 0)
			fprintf(stderr, "[ackaccel] spoof OKAY #%llu (%u,%u) tokens=%d%s\n",
				(unsigned long long)spoofed, out.arg0, out.arg1,
				st.tokens, in.at_boundary() ? "" : " deferred");

		if (in.at_boundary()) {
			flush_pending(out_ti->peer_in);
			enqueue_to(out_ti->peer_in, io);
		} else {
			pending.push_back(io);
		}
	}

	// Submit one device-bound fabricated OKAY. send_data_async() takes
	// ownership of the buffer whatever happens; if the device is gone the
	// IN read loop notices on its next receive and exits the process.
	void submit_to_device(struct thread_info *out_ti, uint8_t *buf)
	{
		send_data_async(out_ti->device_bEndpointAddress, buf, 24,
				USB_REQUEST_TIMEOUT);
	}

	// Caller holds mtx and has checked out.at_boundary().
	void flush_pending_out_locked(struct thread_info *out_ti)
	{
		if (verbose_level > 0 && !pending_out.empty())
			fprintf(stderr, "[ackaccel] flush %zu deferred device-bound spoof(s)\n",
				pending_out.size());
		while (!pending_out.empty()) {
			submit_to_device(out_ti, pending_out.front());
			pending_out.pop_front();
		}
	}

	void on_device_wrte(struct thread_info *in_ti)
	{
		static const uint32_t OKAY = adb_cmd("OKAY");

		// Needs the async submit path (ordering with the OUT read thread's
		// own submissions relies on non-blocking, in-order
		// libusb_submit_transfer calls) and a known device OUT endpoint.
		if (bulk_out_max_in_flight <= 0 || !in_ti->peer_out)
			return;

		auto key = std::make_pair(in.arg0, in.arg1);
		PullPairState &st = pull_pairs[key];

		uint8_t *buf = new uint8_t[24];
		put_le32(buf, OKAY);
		put_le32(buf + 4, in.arg1);	/* host-side stream id */
		put_le32(buf + 8, in.arg0);	/* device-side stream id */
		put_le32(buf + 12, 0);
		put_le32(buf + 16, 0);
		put_le32(buf + 20, OKAY ^ 0xffffffffU);

		if (st.tokens <= 0) {
			// Same rationale as the push-side hold: the host hasn't
			// signalled room since ACK_ACCEL_MAX_AHEAD spoofs, so let
			// real flow control stall the device until its next OKAY.
			fprintf(stderr, "[ackaccel] hold: pull (%u,%u) out of spoof credit, awaiting host OKAY\n",
				in.arg0, in.arg1);
			st.held.push_back(buf);
			return;
		}
		st.tokens--;
		pull_spoofed++;

		if (verbose_level > 0)
			fprintf(stderr, "[ackaccel] pull spoof OKAY #%llu (%u,%u) tokens=%d%s\n",
				(unsigned long long)pull_spoofed, in.arg0, in.arg1,
				st.tokens, out.at_boundary() ? "" : " deferred");

		if (out.at_boundary()) {
			flush_pending_out_locked(in_ti->peer_out);
			submit_to_device(in_ti->peer_out, buf);
		} else {
			pending_out.push_back(buf);
		}
	}

public:
	// Called when endpoints are (re)activated: any previous session's
	// bookkeeping (including queued spoofs holding stale ep numbers) is void.
	void reset()
	{
		std::lock_guard<std::mutex> guard(mtx);
		dead = false;
		out.reset();
		in.reset();
		clear_streams();
		spoofed = swallowed = 0;
		pull_spoofed = pull_swallowed = 0;
	}

	// Feed a host->device bulk read (already committed to being forwarded).
	// Returns true if the accelerator consumed the read (a host OKAY
	// swallowed for the pull direction) and the caller must NOT forward it.
	bool out_feed(struct thread_info *ti, const uint8_t *data, int len)
	{
		static const uint32_t WRTE = adb_cmd("WRTE");
		static const uint32_t CNXN = adb_cmd("CNXN");
		static const uint32_t CLSE = adb_cmd("CLSE");
		static const uint32_t OKAY = adb_cmd("OKAY");

		std::lock_guard<std::mutex> guard(mtx);
		if (dead || !ti->peer_in)
			return false;

		// Pull-direction swallow: a standalone 24-byte host OKAY for a
		// pair we spoof device-bound acks on. Mirrors the device-OKAY
		// fast path in in_process(), including the standalone-transfer
		// assumption (adb writes each transport header as its own USB
		// transfer). The swallowed read never reaches the parser, which
		// is consistent: it never reaches the device either.
		if (len == 24 && out.at_boundary()) {
			uint32_t cmd, arg0, arg1, plen;
			if (parse_hdr(data, &cmd, &arg0, &arg1, &plen) &&
			    cmd == OKAY && plen == 0) {
				auto it = pull_pairs.find(std::make_pair(arg1, arg0));
				if (it != pull_pairs.end()) {
					pull_swallowed++;
					it->second.tokens = ACK_ACCEL_MAX_AHEAD;
					if (!it->second.held.empty())
						fprintf(stderr, "[ackaccel] release %zu held device-bound ack(s) (%u,%u)\n",
							it->second.held.size(), arg1, arg0);
					flush_pending_out_locked(ti);
					while (!it->second.held.empty()) {
						it->second.tokens--;
						pull_spoofed++;
						submit_to_device(ti, it->second.held.front());
						it->second.held.pop_front();
					}
					return true;
				}
				// An OKAY for a pair we never pull-spoofed on (e.g.
				// the push direction's flow control): fall through to
				// the parser and forward as before.
			}
		}

		int off = 0;
		while (off < len) {
			if (out.payload_remaining) {
				uint32_t take = out.payload_remaining;
				if ((int)take > len - off)
					take = len - off;
				out.payload_remaining -= take;
				off += take;
				continue;
			}
			out.hdr[out.hdr_have++] = data[off++];
			if (out.hdr_have < 24)
				continue;
			out.hdr_have = 0;
			uint32_t plen;
			if (!parse_hdr(out.hdr, &out.cmd, &out.arg0, &out.arg1, &plen)) {
				die("unparseable header on host->device stream");
				return false;
			}
			if (verbose_level > 0 && out.cmd != WRTE) {
				char name[5];
				cmd_to_string(out.cmd, name);
				fprintf(stderr, "[ackaccel] host %s (%u,%u) len=%u\n",
					name, out.arg0, out.arg1, plen);
			}
			if (out.cmd == OKAY && !plen && !pull_pairs.empty()) {
				// An OKAY we may owe a swallow for arrived glued to
				// other host data; the standalone assumption broke, so
				// stop meddling rather than desync the bookkeeping.
				die("host OKAY not a standalone read");
				return false;
			}
			if (out.cmd == CNXN) {
				clear_streams();	/* new ADB session */
			} else if (out.cmd == CLSE) {
				pairs.erase(std::make_pair(out.arg0, out.arg1));
				auto pit = pull_pairs.find(std::make_pair(out.arg1, out.arg0));
				if (pit != pull_pairs.end()) {
					for (uint8_t *buf : pit->second.held)
						delete[] buf;
					pull_pairs.erase(pit);
				}
			}
			out.payload_remaining = plen;
			// Ack on the WRTE *header*, not on payload completion: the
			// host is committed to sending the payload and the gadget to
			// accepting it, so the OKAY travels back while the payload is
			// still streaming in and the host can queue the next WRTE
			// with no gap. The at-risk window is bounded the same way
			// (ACK_ACCEL_MAX_AHEAD spoofed-but-unconfirmed WRTEs).
			if (out.cmd == WRTE)
				on_wrte_complete(ti);
		}
		return false;
	}

	// Called by the OUT read thread right after it submitted a read to the
	// device: if that read completed a host->device message, any parked
	// device-bound spoofs may now go out (they land *after* the message
	// they had to wait for).
	void out_flush(struct thread_info *ti)
	{
		std::lock_guard<std::mutex> guard(mtx);
		if (dead || pending_out.empty() || !out.at_boundary())
			return;
		flush_pending_out_locked(ti);
	}

	// How many bytes the next device->host bulk read should request.
	// 0 = no opinion (caller uses its maxPacketSize default). Mid-payload
	// the remaining length is known exactly from the WRTE header, so one
	// read can take the whole payload instead of a blocking libusb call
	// per 512-byte packet — this is what makes pull's datapath cheap.
	// Only the IN read thread advances the `in` parser, so the value can't
	// go stale between this call and the read itself.
	int next_in_read_len()
	{
		std::lock_guard<std::mutex> guard(mtx);
		if (dead || in.payload_remaining == 0)
			return 0;
		uint32_t want = in.payload_remaining;
		if (want > MAX_TRANSFER_SIZE)
			want = MAX_TRANSFER_SIZE;
		return (int)want;
	}

	// Fail-open entry point for the read loops (e.g. a babble/overflow on
	// a sized read means the framing assumption broke).
	void fail(const char *why)
	{
		std::lock_guard<std::mutex> guard(mtx);
		die(why);
	}

	// Feed a device->host bulk read and take charge of enqueueing it
	// (possibly swallowing it). Returns false if the accelerator is inactive
	// and the caller should enqueue the read itself.
	bool in_process(struct thread_info *ti, struct usb_raw_transfer_io &io,
			int nbytes)
	{
		static const uint32_t OKAY = adb_cmd("OKAY");
		static const uint32_t CNXN = adb_cmd("CNXN");
		static const uint32_t CLSE = adb_cmd("CLSE");
		static const uint32_t WRTE = adb_cmd("WRTE");

		std::lock_guard<std::mutex> guard(mtx);
		if (dead)
			return false;

		// The only thing ever swallowed: a standalone 24-byte A_OKAY
		// read whose stream pair has a decision at its FIFO head. adbd
		// writes each transport header as its own USB transfer, so an
		// OKAY always arrives as exactly one 24-byte read (the embedded
		// case below fails open if that ever stops holding).
		if (nbytes == 24 && in.at_boundary()) {
			uint32_t cmd, arg0, arg1, plen;
			if (parse_hdr((uint8_t *)io.data, &cmd, &arg0, &arg1, &plen) &&
			    cmd == OKAY && plen == 0) {
				auto key = std::make_pair(arg1, arg0);
				auto it = pairs.find(key);
				bool drop = false;
				if (it != pairs.end()) {
					// A pair we have spoofed on: the host already
					// got its ack(s), so swallow. The real OKAY
					// means the device drained this stream's
					// buffer (it may cover any number of WRTEs),
					// so refill the spoof credit and release any
					// held ack.
					drop = true;
					swallowed++;
					it->second.tokens = ACK_ACCEL_MAX_AHEAD;
					if (!it->second.held.empty())
						fprintf(stderr, "[ackaccel] release %zu held ack(s) (%u,%u)\n",
							it->second.held.size(), arg0, arg1);
					flush_pending(ti);
					while (!it->second.held.empty()) {
						it->second.tokens--;
						spoofed++;
						enqueue_to(ti, it->second.held.front());
						it->second.held.pop_front();
					}
				} else {
					// Never spoofed on this pair (e.g. the OPEN
					// handshake OKAY): forward untouched.
					if (verbose_level > 0)
						fprintf(stderr, "[ackaccel] device OKAY (%u,%u) forwarded\n",
							arg0, arg1);
					enqueue_to(ti, io);
				}
				flush_pending(ti);
				return true;
			}
		}

		// Generic framing to track message boundaries on the IN stream.
		uint8_t *data = (uint8_t *)io.data;
		int off = 0;
		while (off < nbytes) {
			if (in.payload_remaining) {
				uint32_t take = in.payload_remaining;
				if ((int)take > nbytes - off)
					take = nbytes - off;
				in.payload_remaining -= take;
				off += take;
				continue;
			}
			in.hdr[in.hdr_have++] = data[off++];
			if (in.hdr_have < 24)
				continue;
			in.hdr_have = 0;
			uint32_t plen;
			if (!parse_hdr(in.hdr, &in.cmd, &in.arg0, &in.arg1, &plen)) {
				die("unparseable header on device->host stream");
				enqueue_to(ti, io);
				return true;
			}
			if (in.cmd == OKAY && !plen && !pairs.empty()) {
				// An OKAY we may owe a swallow for arrived glued to
				// other data; the standalone assumption broke, so stop
				// meddling rather than desync the ack bookkeeping.
				die("device OKAY not a standalone read");
				enqueue_to(ti, io);
				return true;
			}
			if (in.cmd == CNXN) {
				clear_streams();
			} else if (in.cmd == CLSE) {
				pairs.erase(std::make_pair(in.arg1, in.arg0));
				auto pit = pull_pairs.find(std::make_pair(in.arg0, in.arg1));
				if (pit != pull_pairs.end()) {
					for (uint8_t *buf : pit->second.held)
						delete[] buf;
					pull_pairs.erase(pit);
				}
			}
			in.payload_remaining = plen;
			// Pull mirror of the push-side ack-on-header: the device is
			// committed to the payload, so ack it device-bound now and
			// let the device queue its next WRTE without waiting for
			// the host's OKAY round trip.
			if (in.cmd == WRTE)
				on_device_wrte(ti);
		}

		enqueue_to(ti, io);
		if (in.at_boundary())
			flush_pending(ti);
		return true;
	}
};

static AdbAckAccel ack_accel;

static uint16_t find_udc_maxpacket_for_interface(uint8_t interface_number)
{
	struct raw_gadget_config *config =
		&host_device_desc.configs[host_device_desc.current_config];
	uint16_t max_limit = 0;

	for (int i = 0; i < config->config.bNumInterfaces; i++) {
		struct raw_gadget_interface *iface = &config->interfaces[i];
		for (int j = 0; j < iface->num_altsettings; j++) {
			struct raw_gadget_altsetting *alt = &iface->altsettings[j];
			if (alt->interface.bInterfaceNumber != interface_number)
				continue;
			for (int k = 0; k < alt->interface.bNumEndpoints; k++) {
				struct raw_gadget_endpoint *ep = &alt->endpoints[k];
				if (usb_endpoint_type(&ep->endpoint) != USB_ENDPOINT_XFER_ISOC)
					continue;
				if (ep->udc_maxpacket_limit &&
				    ep->udc_maxpacket_limit > max_limit)
					max_limit = ep->udc_maxpacket_limit;
			}
		}
	}

	return max_limit;
}

// Translate a gadget-side endpoint address back to the physical device's
// endpoint address. Needed for endpoint-directed class requests (e.g.,
// USB Audio SET_CUR for sampling frequency) when endpoint remapping is active.
static uint16_t remap_endpoint_for_device(uint16_t gadget_ep_addr)
{
	if (!auto_remap_endpoints)
		return gadget_ep_addr;

	struct raw_gadget_config *config =
		&host_device_desc.configs[host_device_desc.current_config];

	for (int i = 0; i < config->config.bNumInterfaces; i++) {
		struct raw_gadget_interface *iface = &config->interfaces[i];
		for (int j = 0; j < iface->num_altsettings; j++) {
			struct raw_gadget_altsetting *alt = &iface->altsettings[j];
			for (int k = 0; k < alt->interface.bNumEndpoints; k++) {
				struct raw_gadget_endpoint *ep = &alt->endpoints[k];
				if (ep->endpoint.bEndpointAddress == (uint8_t)gadget_ep_addr)
					return ep->device_bEndpointAddress;
			}
		}
	}

	return gadget_ep_addr;
}

static void clamp_uvc_probe_commit(const usb_ctrlrequest *ctrl,
				   struct usb_raw_transfer_io &io)
{
	if (!auto_remap_endpoints)
		return;
	if ((ctrl->bRequestType & USB_TYPE_MASK) != USB_TYPE_CLASS)
		return;

	uint8_t interface_number = ctrl->wIndex & 0xff;
	uint8_t selector = ctrl->wValue >> 8;
	if (selector != UVC_VS_PROBE_CONTROL && selector != UVC_VS_COMMIT_CONTROL)
		return;

	uint16_t maxp = find_udc_maxpacket_for_interface(interface_number);
	if (!maxp)
		return;

	if (io.inner.length < UVC_PROBE_MAX_PAYLOAD_OFFSET + 4)
		return;

	uint8_t *payload = (uint8_t *)io.data;
	uint8_t *p = payload + UVC_PROBE_MAX_PAYLOAD_OFFSET;
	uint32_t max_payload = (uint32_t)p[0] |
		((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) |
		((uint32_t)p[3] << 24);
	if (max_payload > maxp) {
		p[0] = maxp & 0xff;
		p[1] = (maxp >> 8) & 0xff;
		p[2] = 0;
		p[3] = 0;
	}
}

static struct raw_gadget_altsetting *find_altsetting(struct raw_gadget_config *config,
						    uint8_t interface_number,
						    uint8_t alt_setting)
{
	for (int i = 0; i < config->config.bNumInterfaces; i++) {
		struct raw_gadget_interface *iface = &config->interfaces[i];
		for (int j = 0; j < iface->num_altsettings; j++) {
			struct raw_gadget_altsetting *alt = &iface->altsettings[j];
			if (alt->interface.bInterfaceNumber == interface_number &&
			    alt->interface.bAlternateSetting == alt_setting)
				return alt;
		}
	}
	return NULL;
}

static struct raw_gadget_endpoint *find_first_streaming_ep(struct raw_gadget_config *config,
							  uint8_t interface_number)
{
	for (int i = 0; i < config->config.bNumInterfaces; i++) {
		struct raw_gadget_interface *iface = &config->interfaces[i];
		for (int j = 0; j < iface->num_altsettings; j++) {
			struct raw_gadget_altsetting *alt = &iface->altsettings[j];
			if (alt->interface.bInterfaceNumber != interface_number)
				continue;
			if (alt->interface.bNumEndpoints > 0)
				return &alt->endpoints[0];
		}
	}
	return NULL;
}

static void rewrite_descriptor_addresses(uint8_t descriptor_type, uint8_t descriptor_index,
					 uint8_t *data, size_t length)
{
	if (!auto_remap_endpoints)
		return;

	if (descriptor_type != USB_DT_CONFIG &&
	    descriptor_type != USB_DT_OTHER_SPEED_CONFIG)
		return;

	if (descriptor_index >= host_device_desc.device.bNumConfigurations)
		return;

	struct raw_gadget_config *config = &host_device_desc.configs[descriptor_index];
	struct raw_gadget_altsetting *current_alt = NULL;
	int current_endpoint = 0;

	size_t offset = 0;
	while (offset + 2 <= length) {
		uint8_t dlen = data[offset];
		if (!dlen)
			break;
		if (offset + dlen > length)
			break;

		uint8_t dtype = data[offset + 1];
		if (dtype == USB_DT_INTERFACE) {
			uint8_t interface_number = data[offset + 2];
			uint8_t alt_setting = data[offset + 3];
			current_alt = find_altsetting(config, interface_number, alt_setting);
			current_endpoint = 0;
		}
		else if (dtype == USB_DT_ENDPOINT) {
			if (current_alt && current_endpoint < current_alt->interface.bNumEndpoints) {
				struct raw_gadget_endpoint *ep = &current_alt->endpoints[current_endpoint];
				data[offset + 2] = ep->endpoint.bEndpointAddress;
				if (offset + 6 < length) {
					uint16_t maxp = ep->endpoint.wMaxPacketSize;
					data[offset + 4] = maxp & 0xff;
					data[offset + 5] = (maxp >> 8) & 0xff;
				}
				// Also rewrite bInterval (offset 6 in endpoint descriptor).
				if (offset + 7 <= length)
					data[offset + 6] = ep->endpoint.bInterval;
				current_endpoint++;
			}
		}
		else if (dtype == USB_DT_CS_INTERFACE) {
			if (current_alt && current_alt->interface.bInterfaceClass == USB_CLASS_VIDEO) {
				uint8_t subtype = data[offset + 2];
				// VideoStreaming Input Header descriptor: bEndpointAddress at offset 6.
				if (subtype == UVC_VS_INPUT_HEADER &&
				    current_alt->interface.bInterfaceSubClass == UVC_SC_VIDEOSTREAMING) {
					if (offset + 6 < length) {
						struct raw_gadget_endpoint *ep = NULL;
						if (current_alt->interface.bNumEndpoints > 0) {
							ep = &current_alt->endpoints[0];
						}
						else {
							ep = find_first_streaming_ep(config,
								current_alt->interface.bInterfaceNumber);
						}
						if (ep)
							data[offset + 6] = ep->endpoint.bEndpointAddress;
					}
				}
			}
		}

		offset += dlen;
	}
}

static void maybe_override_descriptor(struct usb_ctrlrequest *ctrl,
				      struct usb_raw_transfer_io &io)
{
	if (!auto_remap_endpoints)
		return;
	if ((ctrl->bRequestType & USB_TYPE_MASK) != USB_TYPE_STANDARD)
		return;
	if (ctrl->bRequest != USB_REQ_GET_DESCRIPTOR)
		return;

	uint8_t descriptor_type = ctrl->wValue >> 8;
	uint8_t descriptor_index = ctrl->wValue & 0xff;
	rewrite_descriptor_addresses(descriptor_type, descriptor_index,
				     (uint8_t *)io.data, io.inner.length);
}

// Returns the index of the best altsetting that fits UDC limits, or -1 if none.
// The desired_altsetting is returned directly if remapping is disabled or if it has no endpoints.
static int find_best_compatible_altsetting(struct raw_gadget_interface *iface,
					   int desired_interface,
					   int desired_altsetting)
{
	if (!auto_remap_endpoints)
		return desired_altsetting;

	struct raw_gadget_altsetting *desired_alt = &iface->altsettings[desired_altsetting];
	if (desired_alt->interface.bNumEndpoints == 0) {
		// Alt 0 (no endpoints) is usually the idle state; never remap it.
		return desired_altsetting;
	}

	const struct libusb_interface_descriptor *alts =
		device_config_desc[host_device_desc.current_config]
			->interface[desired_interface].altsetting;

	int best_alt = -1;
	int best_packet = -1;

	for (int i = 0; i < iface->num_altsettings; i++) {
		struct raw_gadget_altsetting *alt = &iface->altsettings[i];
		if (alt->interface.bNumEndpoints == 0)
			continue;

		bool fits = true;
		int alt_packet = 0;

		for (int k = 0; k < alt->interface.bNumEndpoints; k++) {
			struct raw_gadget_endpoint *ep = &alt->endpoints[k];
			if (usb_endpoint_type(&ep->endpoint) != USB_ENDPOINT_XFER_ISOC)
				continue;

			uint16_t udc_limit = ep->udc_maxpacket_limit;
			if (!udc_limit)
				continue;

			uint16_t dev_maxp = alts[i].endpoint[k].wMaxPacketSize & 0x7ff;
			if (dev_maxp > udc_limit) {
				fits = false;
				break;
			}
			if (dev_maxp > alt_packet)
				alt_packet = dev_maxp;
		}

		if (fits && alt_packet >= best_packet) {
			best_packet = alt_packet;
			best_alt = i;
		}
	}

	return best_alt;
}

// ── Approach 1: declarative per-byte operations ──────────────────────────────
//
// Applies the "operations" array from an injection rule to the packet in-place.
// Operations are applied in order. Offsets are 0-based.
//
// Supported types (size=1 is int8 default, size=2 is int16 LE):
//   negate  { offset [, size] }           – two's-complement negate signed value
//   scale   { offset, factor [, size] }   – multiply by float, clamp to range
//   add     { offset, value  [, size] }   – add signed constant, clamp to range
//   clamp   { offset, min, max [, size] } – clamp signed value to [min, max]
//   xor     { offset, mask }              – XOR byte with mask (integer)
//   swap    { offset, offset_b }          – swap two bytes
//   copy    { offset, dst_offset }        – copy byte to another position
//   set     { offset, value }             – force byte to unsigned value 0-255
//
static void apply_operations(uint8_t *data, int len, const Json::Value &ops)
{
	for (unsigned int i = 0; i < ops.size(); i++) {
		const Json::Value &op = ops[i];
		std::string type = op.get("type", "").asString();
		int offset = op.get("offset", -1).asInt();

		if (type == "negate") {
			if (offset < 0) continue;
			int size = op.get("size", 1).asInt();
			if (size == 2) {
				if (offset + 1 >= len) continue;
				int32_t v = (int32_t)(int16_t)((uint16_t)data[offset] |
				                               ((uint16_t)data[offset + 1] << 8));
				v = -v;
				if (v < -32768) v = -32768;
				if (v > 32767)  v = 32767;
				uint16_t uv = (uint16_t)(int16_t)v;
				data[offset]     = (uint8_t)(uv & 0xFF);
				data[offset + 1] = (uint8_t)(uv >> 8);
			} else {
				if (offset >= len) continue;
				data[offset] = (uint8_t)(-(int8_t)data[offset]);
			}

		} else if (type == "scale") {
			if (offset < 0) continue;
			double factor = op.get("factor", 1.0).asDouble();
			int size = op.get("size", 1).asInt();
			if (size == 2) {
				if (offset + 1 >= len) continue;
				int32_t v = (int32_t)(int16_t)((uint16_t)data[offset] |
				                               ((uint16_t)data[offset + 1] << 8));
				v = (int32_t)(v * factor);
				if (v < -32768) v = -32768;
				if (v > 32767)  v = 32767;
				uint16_t uv = (uint16_t)(int16_t)v;
				data[offset]     = (uint8_t)(uv & 0xFF);
				data[offset + 1] = (uint8_t)(uv >> 8);
			} else {
				if (offset >= len) continue;
				int result = (int)((int8_t)data[offset] * factor);
				result = std::max(-128, std::min(127, result));
				data[offset] = (uint8_t)(int8_t)result;
			}

		} else if (type == "add") {
			if (offset < 0) continue;
			int size = op.get("size", 1).asInt();
			if (size == 2) {
				if (offset + 1 >= len) continue;
				int32_t v = (int32_t)(int16_t)((uint16_t)data[offset] |
				                               ((uint16_t)data[offset + 1] << 8));
				v += op.get("value", 0).asInt();
				if (v < -32768) v = -32768;
				if (v > 32767)  v = 32767;
				uint16_t uv = (uint16_t)(int16_t)v;
				data[offset]     = (uint8_t)(uv & 0xFF);
				data[offset + 1] = (uint8_t)(uv >> 8);
			} else {
				if (offset >= len) continue;
				int result = (int)(int8_t)data[offset] + op.get("value", 0).asInt();
				result = std::max(-128, std::min(127, result));
				data[offset] = (uint8_t)(int8_t)result;
			}

		} else if (type == "clamp") {
			if (offset < 0) continue;
			int size = op.get("size", 1).asInt();
			if (size == 2) {
				if (offset + 1 >= len) continue;
				int32_t v = (int32_t)(int16_t)((uint16_t)data[offset] |
				                               ((uint16_t)data[offset + 1] << 8));
				int min_v = op.get("min", -32768).asInt();
				int max_v = op.get("max",  32767).asInt();
				v = std::max(min_v, std::min(max_v, (int)v));
				uint16_t uv = (uint16_t)(int16_t)v;
				data[offset]     = (uint8_t)(uv & 0xFF);
				data[offset + 1] = (uint8_t)(uv >> 8);
			} else {
				if (offset >= len) continue;
				int min_v = op.get("min", -128).asInt();
				int max_v = op.get("max",  127).asInt();
				int result = std::max(min_v, std::min(max_v, (int)(int8_t)data[offset]));
				data[offset] = (uint8_t)(int8_t)result;
			}

		} else if (type == "xor") {
			if (offset < 0 || offset >= len) continue;
			data[offset] ^= (uint8_t)op.get("mask", 0).asInt();

		} else if (type == "swap") {
			int b = op.get("offset_b", -1).asInt();
			if (offset < 0 || offset >= len) continue;
			if (b < 0 || b >= len) continue;
			uint8_t tmp = data[offset];
			data[offset] = data[b];
			data[b] = tmp;

		} else if (type == "copy") {
			int dst = op.get("dst_offset", -1).asInt();
			if (offset < 0 || offset >= len) continue;
			if (dst < 0 || dst >= len) continue;
			data[dst] = data[offset];

		} else if (type == "set") {
			if (offset < 0 || offset >= len) continue;
			data[offset] = (uint8_t)op.get("value", 0).asInt();

		} else {
			printf("apply_operations: unknown op type '%s'\n", type.c_str());
		}
	}
}

// ── Approach 2: Lua scripting ─────────────────────────────────────────────────
//
// Each unique script_file gets one lua_State loaded on first use, protected
// by a per-state mutex (Lua states are not thread-safe).
//
// The script must export:
//   function transform(data, len)  →  data, new_len
//
// where `data` is a 1-indexed Lua table of byte values (0-255),
// `len` is the original packet length, and the function returns the
// (possibly modified) table and the new length.
//
#ifdef HAVE_LUA
struct LuaRuleState {
	lua_State *L = nullptr;
	std::mutex call_mutex;
};

static std::mutex                          lua_registry_mutex;
static std::map<std::string, LuaRuleState *> lua_states;

static LuaRuleState *get_lua_state(const std::string &script_file)
{
	std::lock_guard<std::mutex> guard(lua_registry_mutex);
	auto it = lua_states.find(script_file);
	if (it != lua_states.end())
		return it->second;

	auto *state = new LuaRuleState();
	state->L = luaL_newstate();
	luaL_openlibs(state->L);
	if (luaL_dofile(state->L, script_file.c_str()) != LUA_OK) {
		fprintf(stderr, "Lua: failed to load '%s': %s\n",
			script_file.c_str(), lua_tostring(state->L, -1));
		lua_close(state->L);
		state->L = nullptr;
	} else {
		printf("Lua: loaded '%s'\n", script_file.c_str());
	}
	lua_states[script_file] = state;
	return state;
}

static bool apply_lua_transform(const std::string &script_file,
				uint8_t *data, int &len)
{
	LuaRuleState *state = get_lua_state(script_file);
	if (!state || !state->L)
		return false;

	std::lock_guard<std::mutex> guard(state->call_mutex);
	lua_State *L = state->L;

	lua_getglobal(L, "transform");
	if (!lua_isfunction(L, -1)) {
		fprintf(stderr, "Lua: '%s' has no 'transform' function\n",
			script_file.c_str());
		lua_pop(L, 1);
		return false;
	}

	// Build 1-indexed Lua table from packet bytes
	lua_newtable(L);
	for (int i = 0; i < len; i++) {
		lua_pushinteger(L, i + 1);
		lua_pushinteger(L, data[i]);
		lua_rawset(L, -3);
	}
	lua_pushinteger(L, len);

	// Call transform(data, len) → data, new_len
	if (lua_pcall(L, 2, 2, 0) != LUA_OK) {
		fprintf(stderr, "Lua: transform error in '%s': %s\n",
			script_file.c_str(), lua_tostring(L, -1));
		lua_pop(L, 1);
		return false;
	}

	// Second return value: new length
	if (!lua_isnumber(L, -1)) {
		fprintf(stderr, "Lua: '%s' transform must return (table, integer)\n",
			script_file.c_str());
		lua_pop(L, 2);
		return false;
	}
	int new_len = (int)lua_tointeger(L, -1);
	lua_pop(L, 1);

	// First return value: modified byte table
	if (!lua_istable(L, -1)) {
		fprintf(stderr, "Lua: '%s' transform must return (table, integer)\n",
			script_file.c_str());
		lua_pop(L, 1);
		return false;
	}
	new_len = std::min(new_len, MAX_TRANSFER_SIZE);
	for (int i = 0; i < new_len; i++) {
		lua_pushinteger(L, i + 1);
		lua_rawget(L, -2);
		data[i] = (uint8_t)(lua_tointeger(L, -1) & 0xFF);
		lua_pop(L, 1);
	}
	lua_pop(L, 1); // pop table

	len = new_len;
	return true;
}
#endif // HAVE_LUA

// Apply the 3-step injection pipeline (pattern+replace, operations, Lua)
// to the transfer buffer.  Returns true if anything was modified.
static bool apply_injection_pipeline(struct usb_raw_transfer_io &io,
				     const Json::Value &rule)
{
	bool modified = false;

	// Step 1: pattern match + replacement
	if (rule.isMember("content_pattern") && rule.isMember("replacement")) {
		Json::Value patterns = rule["content_pattern"];
		std::string replacement_hex = rule["replacement"].asString();
		if (patterns.size() > 0 && !replacement_hex.empty()) {
			std::string data(io.data, io.inner.length);
			std::string replacement = hexToAscii(replacement_hex);
			for (unsigned int j = 0; j < patterns.size(); j++) {
				std::string pattern_hex = patterns[j].asString();
				std::string pattern = hexToAscii(pattern_hex);

				std::string::size_type pos = data.find(pattern);
				while (pos != std::string::npos) {
					if (data.length() - pattern.length() + replacement.length() > 1023)
						break;
					data = data.replace(pos, pattern.length(), replacement);
					printf("Modified from %s to %s at Index %ld\n",
						pattern_hex.c_str(), replacement_hex.c_str(), pos);
					modified = true;
					pos = data.find(pattern);
				}
			}
			if (modified) {
				io.inner.length = data.length();
				for (size_t j = 0; j < data.length(); j++)
					io.data[j] = data[j];
			}
		}
	}

	// Step 2: declarative operations
	if (rule.isMember("operations") && rule["operations"].size() > 0) {
		apply_operations(reinterpret_cast<uint8_t *>(io.data),
				 (int)io.inner.length,
				 rule["operations"]);
		modified = true;
	}

	// Step 3: Lua transform
#ifdef HAVE_LUA
	if (rule.isMember("script_file")) {
		int len = (int)io.inner.length;
		if (apply_lua_transform(rule["script_file"].asString(),
					reinterpret_cast<uint8_t *>(io.data),
					len)) {
			io.inner.length = (__u32)len;
			modified = true;
		}
	}
#endif

	return modified;
}

// ─────────────────────────────────────────────────────────────────────────────

void injection(struct usb_raw_control_event &event, struct usb_raw_transfer_io &io, int &injection_flags) {
	const std::vector<std::string> injection_type{"modify", "ignore", "stall"};

	for (unsigned int i = 0; i < injection_type.size(); i++) {
		for (unsigned int j = 0; j < injection_config["control"][injection_type[i]].size(); j++) {
			Json::Value rule = injection_config["control"][injection_type[i]][j];
			if (!rule["enable"].asBool())
				continue;

			if (event.ctrl.bRequestType != hexToDecimal(rule["bRequestType"].asInt()) ||
			    event.ctrl.bRequest     != hexToDecimal(rule["bRequest"].asInt()) ||
			    event.ctrl.wValue       != hexToDecimal(rule["wValue"].asInt()) ||
			    event.ctrl.wIndex       != hexToDecimal(rule["wIndex"].asInt()) ||
			    event.ctrl.wLength      != hexToDecimal(rule["wLength"].asInt()))
				continue;

			printf("Matched injection rule: %s, index: %d\n", injection_type[i].c_str(), j);
			if (injection_type[i] == "modify") {
				apply_injection_pipeline(io, rule);
				if (!(event.ctrl.bRequestType & USB_DIR_IN))
					event.ctrl.wLength = io.inner.length;
			}
			else if (injection_type[i] == "ignore") {
				printf("Ignore this control transfer\n");
				injection_flags = USB_INJECTION_FLAG_IGNORE;
			}
			else if (injection_type[i] == "stall") {
				injection_flags = USB_INJECTION_FLAG_STALL;
			}
		}
	}
}

void injection(struct usb_raw_transfer_io &io, __u8 device_ep_address, std::string transfer_type) {
	for (unsigned int i = 0; i < injection_config[transfer_type].size(); i++) {
		Json::Value rule = injection_config[transfer_type][i];
		if (!rule["enable"].asBool() ||
		    hexToDecimal(rule["ep_address"].asInt()) != device_ep_address)
			continue;

		// Snapshot for before/after logging (copy only incurred when verbose)
		uint32_t orig_len = io.inner.length;
		uint8_t orig_data[MAX_TRANSFER_SIZE];
		if (verbose_level >= 1)
			memcpy(orig_data, io.data, orig_len);

		if (apply_injection_pipeline(io, rule)) {
			if (verbose_level >= 1) {
				printf("Injection[%s EP%02x] before:", transfer_type.c_str(), device_ep_address);
				for (uint32_t j = 0; j < orig_len; j++)
					printf(" %02x", orig_data[j]);
				printf("\n");
				printf("Injection[%s EP%02x] after: ", transfer_type.c_str(), device_ep_address);
				for (uint32_t j = 0; j < io.inner.length; j++)
					printf(" %02x", (uint8_t)io.data[j]);
				printf("\n");
			}
			break;
		}
	}
}

void printData(struct usb_raw_transfer_io io, __u8 bEndpointAddress, std::string transfer_type, std::string dir) {
	printf("Sending data to EP%x(%s_%s):", bEndpointAddress,
		transfer_type.c_str(), dir.c_str());
	for (unsigned int i = 0; i < io.inner.length; i++) {
		printf(" %02hhx", (unsigned)io.data[i]);
	}
	printf("\n");
}

void noop_signal_handler(int) { }

// A proxied device can vanish mid-transfer with its final device->host
// response still sitting in a bulk/interrupt IN queue, not yet written to the
// host. The classic case is `fastboot boot`: the bootloader answers OKAY and
// immediately jumps into the kernel, dropping off the bus a fraction of a
// millisecond later. The moment a device-side libusb call then returns
// NO_DEVICE we do want to terminate (see the per-site comments: without udev
// hotplug never fires, so the service manager respawns us on replug) — but
// exiting the instant we notice closes /dev/raw-gadget and takes the gadget
// down with that OKAY still queued. The host's pending status read then fails:
// fastboot reports "Status read failed (Operation timed out)".
//
// So give the IN write threads a brief, bounded window to flush what they
// already hold before we _exit(). Only non-ISO IN queues are drained: ISO
// carries realtime data that is meaningless once the device is gone, and its
// own IN write thread can be the caller here (it must not wait on itself).
// Every thread that reaches this on a NO_DEVICE is a device-side reader or an
// OUT/ISO writer — never a bulk/interrupt IN *write* thread (those only touch
// the gadget, which returns ESHUTDOWN, not NO_DEVICE) — so the threads doing
// the draining are never the ones blocked here, and there is no self-deadlock.
// First caller wins; concurrent callers block in call_once() until it _exit()s.
void drain_in_queues_and_exit(void)
{
	static std::once_flag once;
	std::call_once(once, [] {
		int cfg_idx = host_device_desc.current_config;
		if (cfg_idx < 0) {
			fflush(stdout);
			_exit(0);
		}
		struct raw_gadget_config *config = &host_device_desc.configs[cfg_idx];

		auto in_queues_empty = [&]() {
			for (int i = 0; i < config->config.bNumInterfaces; i++) {
				struct raw_gadget_interface *iface = &config->interfaces[i];
				struct raw_gadget_altsetting *alt =
					&iface->altsettings[iface->current_altsetting];
				for (int k = 0; k < alt->interface.bNumEndpoints; k++) {
					struct raw_gadget_endpoint *ep = &alt->endpoints[k];
					if (!usb_endpoint_dir_in(&ep->endpoint))
						continue;
					if (usb_endpoint_type(&ep->endpoint) == USB_ENDPOINT_XFER_ISOC)
						continue;
					std::mutex *m = ep->thread_info.data_mutex;
					std::deque<usb_raw_transfer_io> *q = ep->thread_info.data_queue;
					if (!m || !q)
						continue;
					std::lock_guard<std::mutex> guard(*m);
					if (!q->empty())
						return false;
				}
			}
			return true;
		};

		// Wait for the queues to empty, then confirm they stay empty across a
		// short settle. Two reasons a single "empty" glimpse is not enough:
		// an OKAY that an IN read thread pulled a hair before the disconnect
		// was noticed elsewhere may still be a few microseconds from being
		// enqueued; and "empty" only means the writer has *popped* the last
		// transfer, so the settle doubles as time for its in-flight
		// usb_raw_ep_write() to actually reach the host.
		const auto deadline = std::chrono::steady_clock::now() +
				      std::chrono::milliseconds(300);
		bool drained = false;
		while (std::chrono::steady_clock::now() < deadline) {
			if (in_queues_empty()) {
				usleep(30000);
				if (in_queues_empty()) {
					drained = true;
					break;
				}
				continue;
			}
			usleep(2000);
		}

		if (verbose_level > 0)
			fprintf(stderr, "[drain] device gone; IN queues %s, exiting\n",
				drained ? "flushed" : "flush timed out");
		fflush(stdout);
		_exit(0);
	});

	// Unreachable in practice: the winning thread above _exit()s and every
	// other caller blocks inside call_once() until it does. Guard anyway so a
	// spurious return can never fall back into a read/write loop.
	for (;;)
		pause();
}

void *ep_loop_write(void *arg) {
	struct thread_info thread_info = *((struct thread_info*) arg);
	int fd = thread_info.fd;
	int ep_num = thread_info.ep_num;
	struct usb_endpoint_descriptor ep = thread_info.endpoint;
	std::string transfer_type = thread_info.transfer_type;
	std::string dir = thread_info.dir;
	std::deque<usb_raw_transfer_io> *data_queue = thread_info.data_queue;
	std::mutex *data_mutex = thread_info.data_mutex;
	std::condition_variable *data_cv = thread_info.data_cv;
	std::atomic<bool> *please_stop = thread_info.please_stop;

	printf("Start writing thread for EP%02x, thread id(%d)\n",
		ep.bEndpointAddress, gettid());

	// Set a no-op handler for SIGUSR1. Sending this signal to the thread
	// will thus interrupt a blocking ioctl call without other side-effects.
	signal(SIGUSR1, noop_signal_handler);

	// Check both per-endpoint flag (interface change) and global flag (device reset)
	while (!*please_stop && !please_stop_eps) {
		assert(ep_num != -1);

		std::unique_lock<std::mutex> lock(*data_mutex);
		if (data_queue->empty()) {
			// Sleep until there is something to write or we are asked
			// to stop. A bare wait_for(1ms) here used to cost 1000
			// wakeups/s per endpoint whether or not any data was
			// flowing - on the appliance that was ~2000 wakeups/s and
			// 3.3% CPU at complete idle, i.e. most of the proxy's idle
			// power draw. The predicate is what wakes us; producers
			// already notify on every push, and terminate_eps()
			// notifies after setting the stop flags. The timeout is
			// only a backstop against a missed notify wedging
			// shutdown, so it can be long.
			data_cv->wait_for(lock, std::chrono::milliseconds(250),
				[&] {
					return !data_queue->empty() ||
						*please_stop || please_stop_eps;
				});
			if (data_queue->empty())
				continue;
		}
		struct usb_raw_transfer_io io = data_queue->front();
		data_queue->pop_front();
		lock.unlock();
		// Wake a reader parked on the queue-full backoff.
		data_cv->notify_all();

		if (verbose_level >= 2)
			printData(io, ep.bEndpointAddress, transfer_type, dir);

		if (ep.bEndpointAddress & USB_DIR_IN) {
			int rv = usb_raw_ep_write(fd, (struct usb_raw_ep_io *)&io);
			if (rv < 0 && errno == ESHUTDOWN) {
				printf("EP%x(%s_%s): device likely reset, stopping thread\n",
					ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
				break;
			}
			if (rv < 0 && errno == EINTR) {
				printf("EP%x(%s_%s): interface likely changing, stopping thread\n",
					ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
				break;
			}
			if (rv < 0 && (errno == EXDEV || errno == ENODATA || errno == EOVERFLOW)) {
				printf("EP%x(%s_%s): isochronous timing error on write (errno=%d), ignoring transfer\n",
					ep.bEndpointAddress, transfer_type.c_str(), dir.c_str(), errno);
				continue;
			}
			if (rv < 0) {
				perror("usb_raw_ep_write()");
				exit(EXIT_FAILURE);
			}
			printf("EP%x(%s_%s): wrote %d bytes to host\n", ep.bEndpointAddress,
				transfer_type.c_str(), dir.c_str(), rv);
		}
		else {
			int length = io.inner.length;
			unsigned char *data = new unsigned char[length];
			memcpy(data, io.data, length);

			if ((ep.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_ISOC) {
				// Mirror the ISO IN read path: call the dedicated ISO function
				// directly rather than going through the send_data() dispatcher.
				// On success the async callback owns and frees the buffer.
				int rv = send_iso_data(thread_info.device_bEndpointAddress,
						       data, length, USB_REQUEST_TIMEOUT);
				if (rv == LIBUSB_ERROR_NO_DEVICE) {
					delete[] data;
					printf("EP%x(%s_%s): device gone, exiting usb-proxy\n",
						ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
					/* The proxied device is gone. libusb hotplug does not fire
					 * without udev, and SIGINT-based shutdown can hang because
					 * the EP0 loop is blocked on the still-connected host side.
					 * Terminate now; the kernel closes /dev/raw-gadget (freeing
					 * the UDC) and the service manager respawns us to re-proxy
					 * on replug. */
					drain_in_queues_and_exit();
					break;
				}
				if (rv != LIBUSB_SUCCESS)
					delete[] data;
			} else if ((ep.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK &&
				   bulk_out_max_in_flight > 0) {
				// Async bulk OUT: submit and keep several transfers in flight so
				// the device-side bus stays busy between packets (see
				// send_data_async). It takes ownership of `data` from here on —
				// the completion callback frees it on success, or it is freed
				// internally on error — so this branch never deletes it.
				int rv = send_data_async(thread_info.device_bEndpointAddress,
							 data, length, USB_REQUEST_TIMEOUT);
				if (rv == LIBUSB_ERROR_NO_DEVICE) {
					printf("EP%x(%s_%s): device gone, exiting usb-proxy\n",
						ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
					/* The proxied device is gone. libusb hotplug does not fire
					 * without udev, and SIGINT-based shutdown can hang because
					 * the EP0 loop is blocked on the still-connected host side.
					 * Terminate now; the kernel closes /dev/raw-gadget (freeing
					 * the UDC) and the service manager respawns us to re-proxy
					 * on replug. */
					drain_in_queues_and_exit();
					break;
				}
			} else {
				int rv = send_data(thread_info.device_bEndpointAddress, ep.bmAttributes,
						   data, length, USB_REQUEST_TIMEOUT);
				if (rv == LIBUSB_ERROR_NO_DEVICE) {
					delete[] data;
					printf("EP%x(%s_%s): device gone, exiting usb-proxy\n",
						ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
					/* The proxied device is gone. libusb hotplug does not fire
					 * without udev, and SIGINT-based shutdown can hang because
					 * the EP0 loop is blocked on the still-connected host side.
					 * Terminate now; the kernel closes /dev/raw-gadget (freeing
					 * the UDC) and the service manager respawns us to re-proxy
					 * on replug. */
					drain_in_queues_and_exit();
					break;
				}
				// send_data() only returns non-SUCCESS for fatal errors now
				// (bulk OUT retries timeouts internally); any failure here
				// means forwarded data was genuinely lost, so say so loudly.
				if (rv != LIBUSB_SUCCESS)
					fprintf(stderr, "EP%x(%s_%s): send_data failed rv=%d (%s), %d bytes lost\n",
						ep.bEndpointAddress, transfer_type.c_str(), dir.c_str(),
						rv, libusb_strerror((libusb_error)rv), length);
				delete[] data;
			}
		}
	}

	printf("End writing thread for EP%02x, thread id(%d)\n",
		ep.bEndpointAddress, gettid());
	return NULL;
}

// A fastboot host download command is exactly "download:%08x" (17 bytes, sent
// as its own bulk OUT transfer). Returns the payload length that follows, or
// -1 if the buffer isn't a download command.
static int64_t fastboot_download_len(const uint8_t *data, int len)
{
	if (len != 17 || memcmp(data, "download:", 9) != 0)
		return -1;
	uint64_t v = 0;
	for (int i = 9; i < 17; i++) {
		uint8_t c = data[i];
		if (c >= '0' && c <= '9')
			v = v * 16 + (c - '0');
		else if (c >= 'a' && c <= 'f')
			v = v * 16 + (c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v = v * 16 + (c - 'A' + 10);
		else
			return -1;
	}
	return (int64_t)v;
}

void *ep_loop_read(void *arg) {
	struct thread_info thread_info = *((struct thread_info*) arg);
	int fd = thread_info.fd;
	int ep_num = thread_info.ep_num;
	struct usb_endpoint_descriptor ep = thread_info.endpoint;
	std::string transfer_type = thread_info.transfer_type;
	std::string dir = thread_info.dir;
	std::deque<usb_raw_transfer_io> *data_queue = thread_info.data_queue;
	std::mutex *data_mutex = thread_info.data_mutex;
	std::condition_variable *data_cv = thread_info.data_cv;
	std::atomic<bool> *please_stop = thread_info.please_stop;

	printf("Start reading thread for EP%02x, thread id(%d)\n",
		ep.bEndpointAddress, gettid());

	// Set a no-op handler for SIGUSR1. Sending this signal to the thread
	// will thus interrupt a blocking ioctl call without other side-effects.
	signal(SIGUSR1, noop_signal_handler);

	// Remaining payload bytes of an in-progress fastboot "download:%08x" on
	// this (bulk OUT) endpoint; used to size the final gadget reads (see the
	// read-sizing comment below). Thread-local by construction: only this
	// thread reads this endpoint, and endpoint threads are torn down and
	// restarted on reset/re-enumeration, which clears the state.
	uint64_t fastboot_dl_remaining = 0;

	// Check both per-endpoint flag (interface change) and global flag (device reset)
	while (!*please_stop && !please_stop_eps) {
		assert(ep_num != -1);
		struct usb_raw_transfer_io io;

		if (ep.bEndpointAddress & USB_DIR_IN) {
			{
				std::unique_lock<std::mutex> lock(*data_mutex);
				if (data_queue->size() >= 32) {
					// Wait for the writer to drain (it notifies
					// after every pop); bounded so please_stop
					// stays responsive.
					data_cv->wait_for(lock, std::chrono::milliseconds(1));
					continue;
				}
			}

			if ((ep.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_ISOC) {
				struct iso_batch_result batch;
				int rv = receive_iso_data_batched(thread_info.device_bEndpointAddress,
								usb_endpoint_maxp(&ep),
								&batch, iso_batch_size, USB_REQUEST_TIMEOUT);
				if (rv == LIBUSB_ERROR_NO_DEVICE) {
					printf("EP%x(%s_%s): device gone, exiting usb-proxy\n",
						ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
					/* The proxied device is gone. libusb hotplug does not fire
					 * without udev, and SIGINT-based shutdown can hang because
					 * the EP0 loop is blocked on the still-connected host side.
					 * Terminate now; the kernel closes /dev/raw-gadget (freeing
					 * the UDC) and the service manager respawns us to re-proxy
					 * on replug. */
					drain_in_queues_and_exit();
					break;
				}

				if (rv != LIBUSB_SUCCESS || !batch.success) {
					if (batch.buffer)
						delete[] batch.buffer;
					continue;
				}

				int packets_enqueued = 0;
				for (int i = 0; i < batch.num_packets; i++) {
					if (batch.packets[i].status != LIBUSB_TRANSFER_COMPLETED) {
						if (verbose_level > 1)
							printf("EP%x(%s_%s): packet %d status %d, skipping\n",
								ep.bEndpointAddress, transfer_type.c_str(),
								dir.c_str(), i, batch.packets[i].status);
						continue;
					}
					if (batch.packets[i].actual_length <= 0)
						continue;

					memcpy(io.data, batch.packets[i].data, batch.packets[i].actual_length);
					io.inner.ep = ep_num;
					io.inner.flags = 0;
					io.inner.length = batch.packets[i].actual_length;

					if (injection_enabled)
						injection(io, thread_info.device_bEndpointAddress, transfer_type);

					data_mutex->lock();
					data_queue->push_back(io);
					data_mutex->unlock();
					data_cv->notify_all();
					packets_enqueued++;
				}
				if (verbose_level)
					printf("EP%x(%s_%s): enqueued %d/%d packets (%d bytes total)\n",
						ep.bEndpointAddress, transfer_type.c_str(), dir.c_str(),
						packets_enqueued, batch.num_packets, batch.total_length);

				if (batch.buffer)
					delete[] batch.buffer;
			}
			else {
				// Non-isochronous: use original single-packet path
				unsigned char *data = NULL;
				int nbytes = -1;

				// With the accelerator's stream parser mid-payload, the
				// exact number of bytes the device is about to send is
				// known from the WRTE header; take the whole remainder in
				// one blocking call instead of one call per 512-byte
				// packet (the dominant per-message cost on adb pull).
				int read_len = 0;
				if (adb_ack_accel &&
				    (ep.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK)
					read_len = ack_accel.next_in_read_len();

				int rv = receive_data(thread_info.device_bEndpointAddress, ep.bmAttributes,
							usb_endpoint_maxp(&ep),
							&data, &nbytes, USB_REQUEST_TIMEOUT,
							read_len);
				if (rv == LIBUSB_ERROR_NO_DEVICE) {
					printf("EP%x(%s_%s): device gone, exiting usb-proxy\n",
						ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
					/* The proxied device is gone. libusb hotplug does not fire
					 * without udev, and SIGINT-based shutdown can hang because
					 * the EP0 loop is blocked on the still-connected host side.
					 * Terminate now; the kernel closes /dev/raw-gadget (freeing
					 * the UDC) and the service manager respawns us to re-proxy
					 * on replug. */
					drain_in_queues_and_exit();
					if (data)
						delete[] data;
					break;
				}

				if (verbose_level > 0)
					fprintf(stderr, "[in] EP%02x receive rv=%d nbytes=%d\n",
						ep.bEndpointAddress, rv, nbytes);

				// A babble on a sized read means the device sent more than
				// the accelerator's parser expected — its framing view is
				// wrong, so stop trusting it (reads fall back to one packet).
				if (rv == LIBUSB_ERROR_OVERFLOW && read_len > 0) {
					fprintf(stderr, "EP%x(%s_%s): overflow on %d-byte sized read, %d bytes\n",
						ep.bEndpointAddress, transfer_type.c_str(), dir.c_str(),
						read_len, nbytes);
					ack_accel.fail("bulk IN overflow on sized read");
				}

				// Only forward a read that actually succeeded. An empty
				// timeout reports nbytes == 0 with no real data; forwarding
				// it would inject a spurious zero-length packet to the host
				// (which breaks the stream on musb). A genuine device ZLP
				// arrives as LIBUSB_SUCCESS with nbytes == 0 and is still
				// forwarded. A sized (multi-packet) read CAN time out with
				// partial data, though — those bytes are real and must be
				// forwarded, not dropped.
				if (rv == LIBUSB_SUCCESS ||
				    (rv == LIBUSB_ERROR_TIMEOUT && nbytes > 0)) {
					power_note_activity(nbytes);
					// Mirror the OUT-side diagnostic on the IN stream so the
					// log shows both halves of the ADB conversation (host
					// WRTE/DATA vs device OKAY) — distinguishes "device never
					// got the data" from "device's ack never reached the host".
					if (adb_bulk_diag &&
					    (ep.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK) {
						if (nbytes > 0)
							adb_bulk_diag_in_state.feed(ep.bEndpointAddress,
										    (uint8_t *)data, nbytes);
						else
							adb_bulk_diag_in_state.zlp(ep.bEndpointAddress);
					}

					memcpy(io.data, data, nbytes);
					io.inner.ep = ep_num;
					io.inner.flags = 0;
					io.inner.length = nbytes;

					if (injection_enabled)
						injection(io, thread_info.device_bEndpointAddress, transfer_type);

					// The accelerator owns the enqueue for bulk IN
					// while active: it may swallow a device OKAY
					// already acked locally, and only inserts
					// fabricated OKAYs at message boundaries.
					bool enqueued_by_accel = adb_ack_accel &&
						(ep.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK &&
						ack_accel.in_process(&thread_info, io, nbytes);
					if (!enqueued_by_accel) {
						data_mutex->lock();
						data_queue->push_back(io);
						data_mutex->unlock();
						data_cv->notify_all();
					}
					if (verbose_level)
						printf("EP%x(%s_%s): enqueued %d bytes to queue\n", ep.bEndpointAddress,
								transfer_type.c_str(), dir.c_str(), nbytes);
				}

				if (data)
					delete[] data;
			}
		}
		else {
			// Bound the OUT queue so a fast host (e.g. a fastboot download)
			// can't outrun the device-side writer and grow the deque without
			// limit. When full, stop reading; the gadget then NAKs the host
			// (USB flow control) until the writer drains. Mirrors the IN-path
			// cap above. With async bulk OUT the writer keeps the device bus
			// busy, so in a healthy transfer this stays near-empty and never
			// actually throttles the host.
			{
				std::unique_lock<std::mutex> lock(*data_mutex);
				if (data_queue->size() >= 64) {
					data_cv->wait_for(lock, std::chrono::milliseconds(1));
					continue;
				}
			}

			io.inner.ep = ep_num;
			io.inner.flags = 0;
			// For ISO OUT, limit the buffer to one packet (wMaxPacketSize).
			// Passing a larger buffer (e.g. 4096) causes musb-hdrc to report
			// req->actual = req->length instead of the real frame size, which
			// then triggers EMSGSIZE (-90) when forwarding to the physical device.
			//
			// musb-hdrc has the same buffer-size sensitivity on bulk/interrupt
			// OUT endpoints: with a multi-packet buffer it used to accept the
			// first transfer but then stall/NAK subsequent OUT data (e.g. the
			// host sends an ADB CNXN header but its follow-up payload never
			// arrives), so OUT reads default to one packet on musb. That
			// stall is most likely the kernel musb requeue-flush bug (fixed
			// by the appliance's 0001 patch); with a fixed kernel,
			// musb_out_read_packets (config/CLI, default 1 = the proven
			// clamp) opts bulk OUT into maxp*N read buffers to cut the
			// per-packet ioctl overhead. Interrupt OUT stays at one packet.
			// Other UDCs (dwc2) have a large RX FIFO and keep the full
			// buffer for throughput.
			if ((ep.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_ISOC) {
				io.inner.length = usb_endpoint_maxp(&ep);
			} else if (gadget_is_musb) {
				unsigned int len = usb_endpoint_maxp(&ep);
				if ((ep.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK)
					len *= musb_out_read_packets;
				if (len > sizeof(io.data))
					len = sizeof(io.data);
				io.inner.length = len;
			} else {
				io.inner.length = sizeof(io.data);
			}

			// Fastboot download tail sizing: the download payload is a raw
			// bulk stream with no terminating ZLP, so when its length is a
			// multiple of wMaxPacketSize the final packets can't complete a
			// multi-packet read via a short packet. If that tail is smaller
			// than the read buffer, usb_raw_ep_read() blocks forever and the
			// download hangs a few KB short of done (seen on musb with
			// multi-packet reads: an 8,026,112-byte image = 1959 full
			// 4096-byte reads + 2048 bytes stuck, host pinned at
			// 'downloading'). The host announces the exact payload length in
			// the "download:%08x" command, so shrink reads to the remaining
			// payload (rounded up to a whole packet) and the last buffer
			// completes on fill instead.
			if ((ep.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK &&
			    fastboot_dl_remaining > 0) {
				unsigned int maxp = usb_endpoint_maxp(&ep);
				uint64_t want = (fastboot_dl_remaining + maxp - 1) / maxp * maxp;
				if (want < io.inner.length)
					io.inner.length = (unsigned int)want;
			}

			int rv = usb_raw_ep_read(fd, (struct usb_raw_ep_io *)&io);
			if (rv < 0 && errno == ESHUTDOWN) {
				printf("EP%x(%s_%s): device likely reset, stopping thread\n",
					ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
				break;
			}
			if (rv < 0 && errno == EINTR) {
				printf("EP%x(%s_%s): interface likely changing, stopping thread\n",
					ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
				break;
			}
			if (rv < 0 && (errno == EXDEV || errno == ENODATA || errno == EOVERFLOW)) {
				if (verbose_level)
					printf("EP%x(%s_%s): isochronous timing error on read (errno=%d), continuing\n",
						ep.bEndpointAddress, transfer_type.c_str(), dir.c_str(), errno);
				continue;
			}
			if (rv < 0) {
				perror("usb_raw_ep_read()");
				exit(EXIT_FAILURE);
			}
			printf("EP%x(%s_%s): read %d bytes from host\n", ep.bEndpointAddress,
					transfer_type.c_str(), dir.c_str(), rv);
			io.inner.length = rv;
			power_note_activity(rv);

			// Advance/arm the fastboot download tracker (see the sizing
			// logic above). A 17-byte "download:%08x" read arms it; payload
			// reads drain it. False positives are near-impossible (an exact
			// 17-byte bulk transfer with that content) and harmless anyway:
			// the tracker only ever shrinks read buffers, never blocks.
			if ((ep.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK &&
			    rv > 0) {
				if (fastboot_dl_remaining > 0) {
					uint64_t got = (uint64_t)rv;
					if (got > fastboot_dl_remaining)
						got = fastboot_dl_remaining;
					fastboot_dl_remaining -= got;
					if (fastboot_dl_remaining == 0)
						printf("EP%x(%s_%s): fastboot download payload complete\n",
							ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
				} else {
					int64_t dl = fastboot_download_len((const uint8_t *)io.data, rv);
					if (dl > 0) {
						fastboot_dl_remaining = (uint64_t)dl;
						printf("EP%x(%s_%s): fastboot download of %lld bytes, sizing tail reads\n",
							ep.bEndpointAddress, transfer_type.c_str(),
							dir.c_str(), (long long)dl);
					}
				}
			}

			if (adb_bulk_diag &&
			    (ep.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK &&
			    rv > 0)
				adb_bulk_diag_state.feed(ep.bEndpointAddress, (uint8_t *)io.data, rv);
			else if (adb_bulk_diag &&
				 (ep.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK &&
				 rv == 0)
				adb_bulk_diag_state.zlp(ep.bEndpointAddress);

			// Optionally drop host bulk transfer-terminator ZLPs instead of
			// forwarding them. The musb one-packet clamp makes us re-chunk each
			// OUT read into its own libusb transfer, so a forwarded 0-length
			// read becomes a spurious 0-length transfer to the device — landing
			// between length-framed protocol messages (e.g. ADB WRTE payloads)
			// and desyncing the device's reader. Length-framed protocols
			// (ADB/fastboot) don't need the ZLP. Only applies to bulk OUT.
			if (rv == 0 && drop_zero_len_out &&
			    (ep.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK) {
				if (verbose_level > 0)
					printf("EP%x(%s_%s): dropping 0-length OUT (ZLP)\n",
						ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
				continue;
			}

			if (injection_enabled)
				injection(io, thread_info.device_bEndpointAddress, transfer_type);

			// From here the read is committed to being forwarded, so the
			// accelerator may ack a WRTE this read completes. It may also
			// consume the read outright (a host OKAY already delivered to
			// the device as a fabricated ack) — then nothing is forwarded.
			if (adb_ack_accel &&
			    (ep.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK &&
			    rv > 0) {
				if (ack_accel.out_feed(&thread_info, (uint8_t *)io.data, rv)) {
					if (verbose_level > 0)
						printf("EP%x(%s_%s): host OKAY swallowed by ack accel\n",
							ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
					continue;
				}
			}

			// Fast path: with async bulk OUT enabled, submit to the device
			// right here instead of handing off to the write thread.
			// libusb_submit_transfer() doesn't block, so this thread returns
			// to usb_raw_ep_read() immediately, and one queue handoff
			// (~100us that sat on the ADB WRTE->OKAY round trip) disappears.
			// All submissions for this endpoint happen on this one thread, so
			// delivery order is preserved; the write thread simply never sees
			// bulk-OUT traffic while the async path is active.
			if ((ep.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK &&
			    bulk_out_max_in_flight > 0) {
				int length = io.inner.length;
				unsigned char *data = new unsigned char[length];
				memcpy(data, io.data, length);

				int arv = send_data_async(thread_info.device_bEndpointAddress,
							  data, length, USB_REQUEST_TIMEOUT);
				if (arv == LIBUSB_ERROR_NO_DEVICE) {
					printf("EP%x(%s_%s): device gone, exiting usb-proxy\n",
						ep.bEndpointAddress, transfer_type.c_str(), dir.c_str());
					/* Same rationale as the write-thread path: no udev, so
					 * hotplug doesn't fire; terminate and let the service
					 * manager respawn us on replug. */
					drain_in_queues_and_exit();
				}
				// If this read completed a host->device message, parked
				// device-bound spoofs may now follow it out.
				if (adb_ack_accel)
					ack_accel.out_flush(&thread_info);
				continue;
			}

			data_mutex->lock();
			data_queue->push_back(io);
			data_mutex->unlock();
			data_cv->notify_all();
			if (verbose_level)
				printf("EP%x(%s_%s): enqueued %d bytes to queue\n", ep.bEndpointAddress,
						transfer_type.c_str(), dir.c_str(), rv);
		}
	}

	printf("End reading thread for EP%02x, thread id(%d)\n",
		ep.bEndpointAddress, gettid());
	return NULL;
}

void process_eps(int fd, int config, int interface, int altsetting) {
	struct raw_gadget_altsetting *alt = &host_device_desc.configs[config]
					.interfaces[interface].altsettings[altsetting];

	printf("Activating %d endpoints on interface %d\n", (int)alt->interface.bNumEndpoints, interface);

	// Endpoint (re)activation voids any previous ADB session bookkeeping.
	ack_accel.reset();

	// Pass 1: set up all endpoint state and enable the endpoints. Threads are
	// only created in pass 2, so cross-endpoint links (peer_in) are complete
	// before any thread can dereference them.
	for (int i = 0; i < alt->interface.bNumEndpoints; i++) {
		struct raw_gadget_endpoint *ep = &alt->endpoints[i];

		int addr = usb_endpoint_num(&ep->endpoint);
		assert(addr != 0);

		ep->thread_info.fd = fd;
		ep->thread_info.endpoint = ep->endpoint;
		ep->thread_info.device_bEndpointAddress = ep->device_bEndpointAddress;
		ep->thread_info.data_queue = new std::deque<usb_raw_transfer_io>;
		ep->thread_info.data_mutex = new std::mutex;
		ep->thread_info.data_cv = new std::condition_variable;
		ep->thread_info.please_stop = new std::atomic<bool>(false);
		ep->thread_info.peer_in = NULL;
		ep->thread_info.peer_out = NULL;

		switch (usb_endpoint_type(&ep->endpoint)) {
		case USB_ENDPOINT_XFER_ISOC:
			ep->thread_info.transfer_type = "isoc";
			break;
		case USB_ENDPOINT_XFER_BULK:
			ep->thread_info.transfer_type = "bulk";
			break;
		case USB_ENDPOINT_XFER_INT:
			ep->thread_info.transfer_type = "int";
			break;
		default:
			printf("transfer_type %d is invalid\n", usb_endpoint_type(&ep->endpoint));
			assert(false);
		}

		if (usb_endpoint_dir_in(&ep->endpoint))
			ep->thread_info.dir = "in";
		else
			ep->thread_info.dir = "out";

		ep->thread_info.ep_num = usb_raw_ep_enable(fd, &ep->thread_info.endpoint);
		printf("%s_%s: addr = %u, ep = #%d\n",
			ep->thread_info.transfer_type.c_str(),
			ep->thread_info.dir.c_str(),
			addr, ep->thread_info.ep_num);
	}

	// Link the altsetting's bulk OUT and bulk IN endpoints to each other:
	// the ADB ACK accelerator queues fabricated OKAYs onto the IN queue
	// (push direction) and submits fabricated OKAYs on the OUT endpoint's
	// device address (pull direction).
	struct thread_info *bulk_in_ti = NULL;
	struct thread_info *bulk_out_ti = NULL;
	for (int i = 0; i < alt->interface.bNumEndpoints; i++) {
		struct raw_gadget_endpoint *ep = &alt->endpoints[i];
		if (usb_endpoint_type(&ep->endpoint) != USB_ENDPOINT_XFER_BULK)
			continue;
		if (usb_endpoint_dir_in(&ep->endpoint))
			bulk_in_ti = &ep->thread_info;
		else
			bulk_out_ti = &ep->thread_info;
	}
	for (int i = 0; i < alt->interface.bNumEndpoints; i++) {
		struct raw_gadget_endpoint *ep = &alt->endpoints[i];
		if (usb_endpoint_type(&ep->endpoint) != USB_ENDPOINT_XFER_BULK)
			continue;
		if (usb_endpoint_dir_in(&ep->endpoint))
			ep->thread_info.peer_out = bulk_out_ti;
		else
			ep->thread_info.peer_in = bulk_in_ti;
	}

	// Pass 2: start the endpoint threads.
	for (int i = 0; i < alt->interface.bNumEndpoints; i++) {
		struct raw_gadget_endpoint *ep = &alt->endpoints[i];

		if (verbose_level)
			printf("Creating thread for EP%02x\n",
				ep->thread_info.endpoint.bEndpointAddress);
		pthread_create(&ep->thread_read, 0,
			ep_loop_read, (void *)&ep->thread_info);
		pthread_create(&ep->thread_write, 0,
			ep_loop_write, (void *)&ep->thread_info);
	}

	printf("process_eps done\n");
}

void terminate_eps(int fd, int config, int interface, int altsetting) {
	struct raw_gadget_altsetting *alt = &host_device_desc.configs[config]
					.interfaces[interface].altsettings[altsetting];

	// Phase 1: Signal all threads to stop and interrupt blocking calls.
	// Set per-endpoint stop flags (not global - only affects this interface's threads).
	// Send SIGUSR1 to interrupt threads blocked on Raw Gadget ioctls.
	// The threads have a no-op handler for this signal, so the ioctl gets
	// interrupted with no other side-effects. Libusb transfer handling
	// does not get interrupted directly and instead times out.
	for (int i = 0; i < alt->interface.bNumEndpoints; i++) {
		struct raw_gadget_endpoint *ep = &alt->endpoints[i];
		if (ep->thread_info.please_stop)
			*ep->thread_info.please_stop = true;
		// Wake the write thread, which sleeps on this condvar until the
		// queue is non-empty or a stop flag is set (the flags above are
		// part of its wait predicate).
		if (ep->thread_info.data_cv)
			ep->thread_info.data_cv->notify_all();
		if (ep->thread_read)
			pthread_kill(ep->thread_read, SIGUSR1);
		if (ep->thread_write)
			pthread_kill(ep->thread_write, SIGUSR1);
	}

	// Phase 2: Wait for all threads to exit.
	for (int i = 0; i < alt->interface.bNumEndpoints; i++) {
		struct raw_gadget_endpoint *ep = &alt->endpoints[i];
		if (ep->thread_read && pthread_join(ep->thread_read, NULL))
			fprintf(stderr, "Error join thread_read\n");
		if (ep->thread_write && pthread_join(ep->thread_write, NULL))
			fprintf(stderr, "Error join thread_write\n");
		ep->thread_read = 0;
		ep->thread_write = 0;
	}

	// Phase 3: Clean up resources after all threads have exited.
	for (int i = 0; i < alt->interface.bNumEndpoints; i++) {
		struct raw_gadget_endpoint *ep = &alt->endpoints[i];
		usb_raw_ep_disable(fd, ep->thread_info.ep_num);
		ep->thread_info.ep_num = -1;

		delete ep->thread_info.data_queue;
		delete ep->thread_info.data_mutex;
		delete ep->thread_info.data_cv;
		delete ep->thread_info.please_stop;
		ep->thread_info.data_queue = nullptr;
		ep->thread_info.data_mutex = nullptr;
		ep->thread_info.data_cv = nullptr;
		ep->thread_info.please_stop = nullptr;
	}
}

void ep0_loop(int fd) {
	bool set_configuration_done_once = false;

	printf("[%.3f] Start for EP0, thread id(%d)\n", uptime_s(), gettid());

	// Run the control loop deaf to incidental signals. USB_RAW_IOCTL_EVENT_FETCH
	// sleeps in the kernel's down_interruptible(), so ANY signal delivered to
	// this thread pops it out with -EINTR (reported as length == UINT_MAX) and
	// tears down an otherwise healthy proxy mid-enumeration -- the power hook's
	// child reaping was seen doing exactly this right after "host connected",
	// wedging the gadget half-configured. Block every asynchronous signal here;
	// leave the synchronous fault signals deliverable (blocking them is undefined
	// if the thread faults) and leave SIGINT/SIGTERM deliverable so a real
	// shutdown still interrupts the fetch. Only this thread's mask changes; the
	// power_monitor thread keeps its own.
	sigset_t block_set;
	sigfillset(&block_set);
	sigdelset(&block_set, SIGINT);
	sigdelset(&block_set, SIGTERM);
	sigdelset(&block_set, SIGSEGV);
	sigdelset(&block_set, SIGBUS);
	sigdelset(&block_set, SIGFPE);
	sigdelset(&block_set, SIGILL);
	sigdelset(&block_set, SIGABRT);
	sigdelset(&block_set, SIGTRAP);
	sigdelset(&block_set, SIGSYS);
	pthread_sigmask(SIG_BLOCK, &block_set, nullptr);

	if (verbose_level)
		print_eps_info(fd);

	while (!please_stop_ep0) {
		struct usb_raw_control_event event;
		event.inner.type = 0;
		event.inner.length = sizeof(event.ctrl);

		usb_raw_event_fetch(fd, (struct usb_raw_event *)&event);
		log_event((struct usb_raw_event *)&event);
		if (event.inner.type == USB_RAW_EVENT_CONNECT)
			printf("[%.3f] host connected to the gadget\n", uptime_s());
		// Control traffic counts as activity too: enumeration and every
		// interface/config change should wind the board up, not wait for
		// the first bulk packet.
		power_note_activity(1);

		if (event.inner.length == 4294967295) {
			// Event fetch interrupted by a signal. With the mask above only
			// SIGINT/SIGTERM reach here, i.e. a real shutdown; log which so
			// the hardware run confirms nothing else is leaking through.
			sigset_t pending;
			sigemptyset(&pending);
			sigpending(&pending);
			printf("End for EP0, thread id(%d) (fetch interrupted; "
			       "SIGINT=%d SIGTERM=%d)\n", gettid(),
			       sigismember(&pending, SIGINT),
			       sigismember(&pending, SIGTERM));
			// Let main()'s pthread_join(hotplug_monitor_thread) complete so the
			// process exits (and inittab respawns) instead of wedging forever.
			please_stop_hotplug_monitor = true;
			return;
		}

		// Normally, we would only need to check for USB_RAW_EVENT_RESET to handle a reset event.
		// However, dwc2 is buggy and it reports a disconnect event instead of a reset.
		if (event.inner.type == USB_RAW_EVENT_RESET || event.inner.type == USB_RAW_EVENT_DISCONNECT) {
			// Normally, we would need to stop endpoint threads first and only then
			// reset the device. However, libusb does not allow interrupting queued
			// requests submitted via sync I/O. Thus, we reset the proxied device to
			// force libusb to interrupt the requests and allow the endpoint threads
			// to exit on please_stop_eps checks.
			if (set_configuration_done_once)
				please_stop_eps = true;
			// Honour reset_device_before_proxy here too: when it is false the
			// proxied device must never be reset (e.g. a device in fastboot mode
			// drops off the bus on a USB reset). musb-hdrc emits spurious
			// disconnect events during host enumeration that would otherwise
			// reset it. When the reset is skipped, the endpoint threads still
			// exit via their libusb transfer timeouts instead of being
			// interrupted by the reset.
			if (reset_device_before_proxy) {
				printf("Resetting device\n");
				reset_device();
			}
			if (set_configuration_done_once) {
				struct raw_gadget_config *config = &host_device_desc.configs[host_device_desc.current_config];
				printf("Stopping endpoint threads\n");
				for (int i = 0; i < config->config.bNumInterfaces; i++) {
					struct raw_gadget_interface *iface = &config->interfaces[i];
					int interface_num = iface->altsettings[iface->current_altsetting]
						.interface.bInterfaceNumber;
					terminate_eps(fd, host_device_desc.current_config, i,
							iface->current_altsetting);
					release_interface(interface_num);
					iface->current_altsetting = 0;
				}
				printf("Endpoint threads stopped\n");
				please_stop_eps = false;
				host_device_desc.current_config = 0;
				set_configuration_done_once = false;
			}
			continue;
		}

		if (event.inner.type != USB_RAW_EVENT_CONTROL)
			continue;

		struct usb_raw_transfer_io io;
		io.inner.ep = 0;
		io.inner.flags = 0;
		io.inner.length = event.ctrl.wLength;

		int injection_flags = USB_INJECTION_FLAG_NONE;
		int nbytes = 0;
		int result = 0;
		unsigned char *control_data = new unsigned char[event.ctrl.wLength];

		// For endpoint-directed class requests, translate the gadget-side
		// endpoint address in wIndex to the physical device's address.
		if ((event.ctrl.bRequestType & USB_RECIP_MASK) == USB_RECIP_ENDPOINT) {
			uint16_t device_ep = remap_endpoint_for_device(event.ctrl.wIndex & 0xff);
			if (device_ep != (event.ctrl.wIndex & 0xff)) {
				printf("ep0: remapping wIndex endpoint 0x%02x -> 0x%02x\n",
					event.ctrl.wIndex & 0xff, device_ep);
				event.ctrl.wIndex = (event.ctrl.wIndex & 0xff00) | device_ep;
			}
		}

		int rv = -1;
		if (event.ctrl.bRequestType & USB_DIR_IN) {
			result = control_request(&event.ctrl, &nbytes, &control_data, USB_REQUEST_TIMEOUT);
			if (result == 0) {
				memcpy(&io.data[0], control_data, nbytes);
				io.inner.length = nbytes;

				if (injection_enabled) {
					injection(event, io, injection_flags);
					switch(injection_flags) {
					case USB_INJECTION_FLAG_NONE:
						break;
					case USB_INJECTION_FLAG_IGNORE:
						delete[] control_data;
						continue;
					case USB_INJECTION_FLAG_STALL:
						delete[] control_data;
						usb_raw_ep0_stall(fd);
						continue;
					default:
						printf("[Warning] Unknown injection flags: %d\n", injection_flags);
						break;
					}
				}

				maybe_override_descriptor(&event.ctrl, io);
				clamp_uvc_probe_commit(&event.ctrl, io);

				// Some UDCs require bMaxPacketSize0 to be at least 64.
				// Ideally, the information about UDC limitations needs to be
				// exposed by Raw Gadget, but this is not implemented at the moment;
				// see https://github.com/xairy/raw-gadget/issues/41.
				if (bmaxpacketsize0_must_greater_than_64 &&
				    (event.ctrl.bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD &&
				    event.ctrl.bRequest == USB_REQ_GET_DESCRIPTOR &&
				    (event.ctrl.wValue >> 8) == USB_DT_DEVICE) {
					struct usb_device_descriptor *dev = (struct usb_device_descriptor *)&io.data;
					if (dev->bMaxPacketSize0 < 64)
						dev->bMaxPacketSize0 = 64;
				}

				if (verbose_level >= 2)
					printData(io, 0x00, "control", "in");

				rv = usb_raw_ep0_write(fd, (struct usb_raw_ep_io *)&io);
				if (rv < 0)
					printf("ep0: ack failed: %d\n", rv);
				else
					printf("ep0: transferred %d bytes (in)\n", rv);
			}
			else {
				// The device dropping off the bus while only control traffic
				// is in flight (host still enumerating, or right around
				// SET_CONFIGURATION before any endpoint thread exists) used
				// to leave the proxy stalling ep0 forever with a dead handle
				// and the gadget still attached: no endpoint thread was there
				// to hit NO_DEVICE and _exit. Exit here like they do.
				if (result == LIBUSB_ERROR_NO_DEVICE) {
					printf("ep0: device gone, exiting usb-proxy\n");
					drain_in_queues_and_exit();
				}
				usb_raw_ep0_stall(fd);
				continue;
			}
		}
		else {
			if ((event.ctrl.bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD &&
					event.ctrl.bRequest == USB_REQ_SET_CONFIGURATION) {
				int desired_config = -1;
				for (int i = 0; i < host_device_desc.device.bNumConfigurations; i++) {
					if (host_device_desc.configs[i].config.bConfigurationValue == event.ctrl.wValue) {
						desired_config = i;
						break;
					}
				}
				if (desired_config < 0) {
					printf("[Warning] Skip changing configuration, wValue(%d) is invalid\n", event.ctrl.wValue);
					continue;
				}

				struct raw_gadget_config *config = &host_device_desc.configs[desired_config];

				if (set_configuration_done_once) { // Need to stop all threads for eps and cleanup
					printf("Changing configuration\n");
					for (int i = 0; i < config->config.bNumInterfaces; i++) {
						struct raw_gadget_interface *iface = &config->interfaces[i];
						int interface_num = iface->altsettings[iface->current_altsetting]
							.interface.bInterfaceNumber;
						terminate_eps(fd, host_device_desc.current_config, i,
								iface->current_altsetting);
						release_interface(interface_num);
					}
				}

				printf("[%.3f] host SET_CONFIGURATION %d\n", uptime_s(), event.ctrl.wValue);
				usb_raw_configure(fd);
				set_configuration(config->config.bConfigurationValue);
				host_device_desc.current_config = desired_config;

				for (int i = 0; i < config->config.bNumInterfaces; i++) {
					struct raw_gadget_interface *iface = &config->interfaces[i];
					iface->current_altsetting = 0;
					int interface_num = iface->altsettings[0].interface.bInterfaceNumber;
					claim_interface(interface_num);
					process_eps(fd, desired_config, i, 0);
					usleep(10000); // Give threads time to spawn.
				}

				set_configuration_done_once = true;

				// Ack request after spawning endpoint threads.
				rv = usb_raw_ep0_read(fd, (struct usb_raw_ep_io *)&io);
				if (rv < 0)
					printf("ep0: ack failed: %d\n", rv);
				else
					printf("ep0: request acked\n");
			}
			else if ((event.ctrl.bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD &&
					event.ctrl.bRequest == USB_REQ_SET_INTERFACE) {
				struct raw_gadget_config *config =
					&host_device_desc.configs[host_device_desc.current_config];

				int desired_interface = -1;
				for (int i = 0; i < config->config.bNumInterfaces; i++) {
					if (config->interfaces[i].altsettings[0].interface.bInterfaceNumber ==
							event.ctrl.wIndex) {
						desired_interface = i;
						break;
					}
				}
				if (desired_interface < 0) {
					printf("[Warning] Skip changing interface, wIndex(%d) is invalid\n", event.ctrl.wIndex);
					continue;
				}

				struct raw_gadget_interface *iface = &config->interfaces[desired_interface];

				int desired_altsetting = -1;
				for (int i = 0; i < iface->num_altsettings; i++) {
					if (iface->altsettings[i].interface.bAlternateSetting == event.ctrl.wValue) {
						desired_altsetting = i;
						break;
					}
				}
				if (desired_altsetting < 0) {
					printf("[Warning] Skip changing alt_setting, wValue(%d) is invalid\n", event.ctrl.wValue);
					continue;
				}

				int effective_altsetting = find_best_compatible_altsetting(
					iface, desired_interface, desired_altsetting);

				if (effective_altsetting < 0) {
					printf("[Warning] No compatible altsetting for interface %d, stalling\n",
						iface->altsettings[desired_altsetting].interface.bInterfaceNumber);
					usb_raw_ep0_stall(fd);
					continue;
				}

				if (effective_altsetting != desired_altsetting) {
					printf("[Warning] Altsetting %d exceeds UDC limit; using %d instead\n",
						iface->altsettings[desired_altsetting].interface.bAlternateSetting,
						iface->altsettings[effective_altsetting].interface.bAlternateSetting);
				}

				struct raw_gadget_altsetting *alt = &iface->altsettings[effective_altsetting];

				if (effective_altsetting == iface->current_altsetting) {
					printf("Interface/altsetting already set\n");
					// But lets propagate the request to the device.
					set_interface_alt_setting(alt->interface.bInterfaceNumber,
						alt->interface.bAlternateSetting);
				}
				else {
					printf("Changing interface/altsetting\n");
					terminate_eps(fd, host_device_desc.current_config,
						desired_interface, iface->current_altsetting);
					set_interface_alt_setting(alt->interface.bInterfaceNumber,
						alt->interface.bAlternateSetting);
					process_eps(fd, host_device_desc.current_config,
						desired_interface, effective_altsetting);
					iface->current_altsetting = effective_altsetting;
					usleep(10000); // Give threads time to spawn.
				}

				// Ack request after spawning endpoint threads.
				rv = usb_raw_ep0_read(fd, (struct usb_raw_ep_io *)&io);
				if (rv < 0)
					printf("ep0: ack failed: %d\n", rv);
				else
					printf("ep0: request acked\n");
			}
			else {
				if (injection_enabled) {
					injection(event, io, injection_flags);
					switch(injection_flags) {
					case USB_INJECTION_FLAG_NONE:
						break;
					case USB_INJECTION_FLAG_IGNORE:
						delete[] control_data;
						continue;
					case USB_INJECTION_FLAG_STALL:
						delete[] control_data;
						usb_raw_ep0_stall(fd);
						continue;
					default:
						printf("[Warning] Unknown injection flags: %d\n", injection_flags);
						break;
					}
				}

				if (event.ctrl.wLength == 0) {
					// For 0-length request, we can ack or stall the request via
					// Raw Gadget, depending on what the proxied device does.

					if (verbose_level >= 2)
						printData(io, 0x00, "control", "out");

					result = control_request(&event.ctrl, &nbytes, &control_data, USB_REQUEST_TIMEOUT);
					if (result == 0) {
						// Ack the request.
						rv = usb_raw_ep0_read(fd, (struct usb_raw_ep_io *)&io);
						if (rv < 0)
							printf("ep0: ack failed: %d\n", rv);
						else
							printf("ep0: request acked\n");
					}
					else {
						if (result == LIBUSB_ERROR_NO_DEVICE) {
							printf("ep0: device gone, exiting usb-proxy\n");
							drain_in_queues_and_exit();
						}
						// Stall the request.
						usb_raw_ep0_stall(fd);
						continue;
					}
				}
				else {
					// For non-0-length requests, we cannot retrieve the request data
					// without acking the request due to the Gadget subsystem limitations.
					// Thus, we cannot stall such request for the host even if the proxied
					// device stalls. This is not ideal but seems to work fine in practice.

					// Retrieve data for sending request to proxied device
					// (and ack the request).
					rv = usb_raw_ep0_read(fd, (struct usb_raw_ep_io *)&io);
					if (rv < 0) {
						printf("ep0: ack failed: %d\n", rv);
						continue;
					}

					if (verbose_level >= 2)
						printData(io, 0x00, "control", "out");

					clamp_uvc_probe_commit(&event.ctrl, io);
					memcpy(control_data, io.data, event.ctrl.wLength);

					result = control_request(&event.ctrl, &nbytes, &control_data, USB_REQUEST_TIMEOUT);
					if (result == 0) {
						printf("ep0: transferred %d bytes (out)\n", rv);
					}
					else if (result == LIBUSB_ERROR_NO_DEVICE) {
						printf("ep0: device gone, exiting usb-proxy\n");
						drain_in_queues_and_exit();
					}
				}
			}
		}

		delete[] control_data;
	}

	struct raw_gadget_config *config = &host_device_desc.configs[host_device_desc.current_config];

	for (int i = 0; i < config->config.bNumInterfaces; i++) {
		struct raw_gadget_interface *iface = &config->interfaces[i];
		int interface_num = iface->altsettings[iface->current_altsetting]
			.interface.bInterfaceNumber;
		terminate_eps(fd, host_device_desc.current_config, i,
				iface->current_altsetting);
		release_interface(interface_num);
	}

	printf("End for EP0, thread id(%d)\n", gettid());
}

/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared raw-mode receive path for the radio testbed images.
 *
 * Only compiled under CONFIG_IEEE802154_RAW_MODE (see CMakeLists.txt): raw
 * mode leaves net_recv_data() undefined, so this file provides it for the
 * rf2xx driver. Non-raw images (e.g. the gateway custom L2) get
 * net_recv_data() from the net stack and must NOT compile this file.
 *
 * Receive path responsibilities:
 *   1. classify every frame (ok / foreign / malformed) so neighbour traffic
 *      does not look like link loss;
 *   2. keep the per-frame data (RSSI/LQI/payload) in a ring buffer so it can
 *      be exported in bulk without depending on the UART keeping up;
 *   3. update the target-side link statistics that survive serial loss.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/spinlock.h>

#include <zephyr/net_buf.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/ieee802154_pkt.h>

#include <zephyr/sys/atomic.h>

#include <app/lib/sensor_frame.h>
#include <app/lib/radio_testbed.h>

LOG_MODULE_REGISTER(radio_testbed, LOG_LEVEL_INF);

#if DT_HAS_ALIAS(led0)
static const struct gpio_dt_spec rx_led =
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#endif
static bool led_ready;
static atomic_t rx_ok;
static atomic_t rx_dropped;
static atomic_t rx_foreign;
static atomic_t rx_malformed;

/* Single-writer (RX thread) link stats; the app/command thread only reads
 * them, so plain words are fine alongside the atomic counters. */
static bool link_has_seq;
static uint16_t link_last_seq;
static uint32_t link_gaps;
static uint32_t link_dups;
static int16_t link_rssi_min;
static int16_t link_rssi_max;
static int32_t link_rssi_sum;
static uint32_t link_lqi_min;
static uint32_t link_lqi_max;
static uint32_t link_lqi_sum;

/* All-zero by default: accept every decodable sensor frame (historic
 * promiscuous testbed behaviour). radio-rx-testbed calls the setter. */
static struct radio_testbed_filter rx_filter;

/*
 * On-target ring of per-frame records.
 *
 * The RX thread is the only writer; the console `dump` command is the only
 * reader. A spinlock guards the slot copy/head update; it is held for a
 * single struct copy, so the RX thread never blocks meaningfully. The record
 * is a fixed, self-describing snapshot so the host can rebuild per-seq/RSSI
 * statistics offline even if the live UART log dropped lines.
 */
#if defined(CONFIG_RADIO_TESTBED_RING)
#define RING_SIZE CONFIG_RADIO_TESTBED_RING_SIZE

struct ring_record {
	uint32_t index;       /* global append index, see radio_testbed_ring_total() */
	uint32_t uptime_ms;   /* receiver clock at reception */
	uint16_t seq;         /* app seq for OK frames, 0 otherwise */
	uint16_t light;       /* decoded payload (OK frames only) */
	int16_t  temp_c_x100;
	int16_t  accel[3];
	uint8_t  node_id;
	uint8_t  flags;
	int8_t   rssi;        /* dBm */
	uint8_t  lqi;
	uint8_t  status;      /* enum radio_testbed_rx_status */
};

static struct ring_record ring[RING_SIZE];
static uint16_t ring_head;    /* slot the next append writes */
static uint32_t ring_total;   /* cumulative appends since boot */
static struct k_spinlock ring_lock;

static void ring_append(const struct ring_record *rec)
{
	k_spinlock_key_t key = k_spin_lock(&ring_lock);

	ring[ring_head] = *rec;
	/* Stamp the slot with its own global index *inside* the lock: a dump
	 * that is being overwritten mid-read can then be recognised by the
	 * host from the index it carries, instead of being filed under the
	 * wrong position and silently corrupting the per-seq accounting. */
	ring[ring_head].index = ring_total;
	ring_head = (uint16_t)((ring_head + 1U) % RING_SIZE);
	ring_total++;
	k_spin_unlock(&ring_lock, key);
}

static uint32_t ring_used(void)
{
	return ring_total < RING_SIZE ? ring_total : (uint32_t)RING_SIZE;
}

uint32_t radio_testbed_ring_total(void)
{
	return ring_total;
}

uint32_t radio_testbed_ring_count(void)
{
	return ring_used();
}

void radio_testbed_ring_reset(void)
{
	k_spinlock_key_t key = k_spin_lock(&ring_lock);

	ring_head = 0;
	ring_total = 0;
	k_spin_unlock(&ring_lock, key);
}
#else
uint32_t radio_testbed_ring_total(void)
{
	return 0;
}

uint32_t radio_testbed_ring_count(void)
{
	return 0;
}

void radio_testbed_ring_reset(void)
{
}
#endif /* CONFIG_RADIO_TESTBED_RING */

int radio_testbed_led_init(void)
{
#if DT_HAS_ALIAS(led0)
	int ret;

	if (!gpio_is_ready_dt(&rx_led)) {
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&rx_led, GPIO_OUTPUT_INACTIVE);
	if (ret == 0) {
		led_ready = true;
	}

	return ret;
#else
	return -ENODEV;
#endif
}

void radio_testbed_stats(uint32_t *ok, uint32_t *dropped)
{
	if (ok != NULL) {
		*ok = (uint32_t)atomic_get(&rx_ok);
	}
	if (dropped != NULL) {
		*dropped = (uint32_t)atomic_get(&rx_dropped);
	}
}

void radio_testbed_link_stats(struct radio_testbed_link_stats *out)
{
	if (out == NULL) {
		return;
	}
	out->ok = (uint32_t)atomic_get(&rx_ok);
	out->foreign = (uint32_t)atomic_get(&rx_foreign);
	out->malformed = (uint32_t)atomic_get(&rx_malformed);
	out->dropped = out->foreign + out->malformed;
	out->gaps = link_gaps;
	out->dups = link_dups;
	out->last_seq = link_last_seq;
	out->has_seq = link_has_seq;
	out->rssi_min = link_rssi_min;
	out->rssi_max = link_rssi_max;
	out->rssi_sum = link_rssi_sum;
	out->lqi_min = link_lqi_min;
	out->lqi_max = link_lqi_max;
	out->lqi_sum = link_lqi_sum;
}

void radio_testbed_set_filter(const struct radio_testbed_filter *filter)
{
	if (filter == NULL) {
		(void)memset(&rx_filter, 0, sizeof(rx_filter));
	} else {
		rx_filter = *filter;
	}
}

void radio_testbed_reset_stats(void)
{
	atomic_set(&rx_ok, 0);
	atomic_set(&rx_dropped, 0);
	atomic_set(&rx_foreign, 0);
	atomic_set(&rx_malformed, 0);
	link_has_seq = false;
	link_last_seq = 0;
	link_gaps = 0;
	link_dups = 0;
	link_rssi_min = 0;
	link_rssi_max = 0;
	link_rssi_sum = 0;
	link_lqi_min = 0;
	link_lqi_max = 0;
	link_lqi_sum = 0;
}

/* True when the frame header matches the configured filter. With no filter
 * configured everything matches, which keeps the testbed's fail-open
 * "show me something" behaviour. */
static bool frame_is_ours(const struct sensor_frame_meta *m)
{
	if (rx_filter.check_pan) {
		uint16_t pan;

		if (m->has_dst_pan) {
			pan = m->dst_pan_id;
		} else if (m->has_src_pan) {
			pan = m->src_pan_id;
		} else {
			/* No PAN field at all: cannot be our PAN network. */
			return false;
		}
		if (pan != rx_filter.pan_id) {
			return false;
		}
	}

	if (rx_filter.check_src) {
		if (!m->src_is_short ||
		    m->src_short_addr != rx_filter.src_short_addr) {
			return false;
		}
	}

	return true;
}

/* Log up to HEXDUMP_LEN payload bytes so a silent link still tells us *what*
 * arrived (wrong PAN, foreign traffic, truncated frame, unstripped FCS).
 * Bounded: one WRN line, no packet data kept. Rate-limited: foreign/malformed
 * frames can arrive continuously on a busy channel; log the first few and
 * then every 64th so the UART is not swamped (the counters and ring keep the
 * full picture either way). */
#define RX_HEXDUMP_LEN 16
static void rx_log_rejected(enum radio_testbed_rx_status status,
			    const struct sensor_frame_meta *meta, bool have_hdr,
			    const uint8_t *data, size_t len, uint32_t dropped)
{
	char hex[RX_HEXDUMP_LEN * 3];
	const char *what = (status == RADIO_TESTBED_RX_FOREIGN) ? "foreign"
								: "malformed";
	size_t n, i, pos = 0;

	if (dropped > 4U && (dropped % 64U) != 0U) {
		return;
	}

	if (data == NULL) {
		LOG_WRN("rx %s frame (no buffer), dropped=%u", what, dropped);
		return;
	}

	n = len < RX_HEXDUMP_LEN ? len : RX_HEXDUMP_LEN;
	for (i = 0; i < n && pos + 3 < sizeof(hex); i++) {
		pos += (size_t)snprintk(&hex[pos], sizeof(hex) - pos,
					"%02x ", data[i]);
	}
	hex[pos] = '\0';

	if (have_hdr && meta != NULL) {
		LOG_WRN("rx %s frame (%d B, pan=0x%04x src=%s0x%04x seq=%u, "
			"head: %s), dropped=%u",
			what, (int)len,
			meta->has_dst_pan ? meta->dst_pan_id : meta->src_pan_id,
			meta->src_is_short ? "" : "ext", meta->src_short_addr,
			meta->mac_seq, hex, dropped);
	} else {
		LOG_WRN("rx %s frame (%d B, head: %s), dropped=%u",
			what, (int)len, hex, dropped);
	}
}

#if defined(CONFIG_RADIO_TESTBED_RING)
static void ring_append_event(enum radio_testbed_rx_status status,
			      const struct sensor_frame_meta *meta, bool have_hdr,
			      const struct sensor_reading *r, int rssi,
			      unsigned int lqi)
{
	struct ring_record rec = {
		.uptime_ms = k_uptime_get_32(),
		.rssi = (int8_t)rssi,
		.lqi = (uint8_t)lqi,
		.status = (uint8_t)status,
	};

	if (r != NULL) {
		rec.seq = r->seq;
		rec.node_id = r->node_id;
		rec.flags = r->flags;
		rec.light = r->light;
		rec.temp_c_x100 = r->temp_c_x100;
		rec.accel[0] = r->accel[0];
		rec.accel[1] = r->accel[1];
		rec.accel[2] = r->accel[2];
	} else if (have_hdr && meta != NULL) {
		/* Foreign/malformed data frame: keep the MAC seq so the host can
		 * at least see how much neighbour traffic there was. */
		rec.seq = meta->mac_seq;
	}

	ring_append(&rec);
}
#else
static void ring_append_event(enum radio_testbed_rx_status status,
			      const struct sensor_frame_meta *meta, bool have_hdr,
			      const struct sensor_reading *r, int rssi,
			      unsigned int lqi)
{
	ARG_UNUSED(status);
	ARG_UNUSED(meta);
	ARG_UNUSED(have_hdr);
	ARG_UNUSED(r);
	ARG_UNUSED(rssi);
	ARG_UNUSED(lqi);
}
#endif

void radio_testbed_dump(void)
{
#if defined(CONFIG_RADIO_TESTBED_RING)
	uint32_t total, count, start, i;

	/* Snapshot the extent of the dump under one lock so `count` and the
	 * `start` slot are derived from the same ring state. */
	{
		k_spinlock_key_t key = k_spin_lock(&ring_lock);

		total = ring_total;
		count = ring_used();
		start = (uint32_t)(ring_head + RING_SIZE - count) % RING_SIZE;
		k_spin_unlock(&ring_lock, key);
	}

	LOG_INF("BULK_BEGIN total=%u count=%u", total, count);
	log_flush();

	for (i = 0; i < count; i++) {
		struct ring_record rec;
		k_spinlock_key_t key = k_spin_lock(&ring_lock);

		rec = ring[(start + i) % RING_SIZE];
		k_spin_unlock(&ring_lock, key);

		/* `idx` is the record's own global append index, not the
		 * position in this dump: the reader can be slower than the
		 * writer, and a slot overwritten while we stream is then
		 * detected as a hole in the index sequence rather than
		 * silently attributed to its neighbour's sequence. */
		LOG_INF("BULK idx=%u seq=%u node=%u status=%u rssi=%d lqi=%u "
			"light=%u temp=%d ax=%d ay=%d az=%d flags=%u uptime=%u",
			rec.index, rec.seq, rec.node_id, rec.status, rec.rssi,
			rec.lqi, rec.light, rec.temp_c_x100,
			rec.accel[0], rec.accel[1], rec.accel[2],
			rec.flags, rec.uptime_ms);
		/* The deferred log queue is far smaller than a full ring; flush per
		 * record so a dump cannot overflow it and silently lose rows. */
		log_flush();
	}

	LOG_INF("BULK_END total=%u count=%u", total, count);
	log_flush();
#else
	LOG_WRN("ring buffer disabled (CONFIG_RADIO_TESTBED_RING=n)");
#endif
}

/* Called by the rf2xx driver RX thread for every received frame. Runs in
 * driver context: decode fast, never block, always unref the pkt.
 */
int net_recv_data(struct net_if *iface, struct net_pkt *pkt)
{
	enum radio_testbed_rx_status status;
	struct sensor_frame_meta meta = { 0 };
	struct sensor_reading r;
	struct net_buf *frag;
	uint32_t dropped;
	bool decoded;
	bool have_hdr;
	int rssi;
	unsigned int lqi;

	ARG_UNUSED(iface);

	if (pkt == NULL) {
		return -EINVAL;
	}

	if (!led_ready) {
		/* Fail-open lazy init in case the app never called it. */
		(void)radio_testbed_led_init();
	}

	frag = net_buf_frag_last(pkt->buffer);
	if (frag == NULL) {
		atomic_inc(&rx_malformed);
		atomic_inc(&rx_dropped);
		net_pkt_unref(pkt);
		return 0;
	}

	decoded = (sensor_frame_decode_meta(frag->data, frag->len, &r, &meta) == 0);
	have_hdr = decoded ||
		   (sensor_frame_parse_header(frag->data, frag->len, &meta) == 0);

	if (decoded) {
		status = frame_is_ours(&meta) ? RADIO_TESTBED_RX_OK
					      : RADIO_TESTBED_RX_FOREIGN;
	} else if (have_hdr) {
		/* Well-formed 802.15.4 data frame, but not our payload: foreign
		 * if it belongs to another PAN/address, otherwise malformed. */
		status = frame_is_ours(&meta) ? RADIO_TESTBED_RX_MALFORMED
					      : RADIO_TESTBED_RX_FOREIGN;
	} else {
		status = RADIO_TESTBED_RX_MALFORMED;
	}

	rssi = (int)net_pkt_ieee802154_rssi_dbm(pkt);
	lqi = (unsigned int)net_pkt_ieee802154_lqi(pkt);

	switch (status) {
	case RADIO_TESTBED_RX_OK:
		atomic_inc(&rx_ok);
		break;
	case RADIO_TESTBED_RX_FOREIGN:
		atomic_inc(&rx_foreign);
		atomic_inc(&rx_dropped);
		break;
	default:
		atomic_inc(&rx_malformed);
		atomic_inc(&rx_dropped);
		break;
	}

	ring_append_event(status, &meta, have_hdr, decoded ? &r : NULL,
			  rssi, lqi);

	if (status != RADIO_TESTBED_RX_OK) {
		dropped = (uint32_t)atomic_get(&rx_dropped);
		rx_log_rejected(status, &meta, have_hdr, frag->data, frag->len,
				dropped);
		net_pkt_unref(pkt);
		return 0;
	}

#if DT_HAS_ALIAS(led0)
	if (led_ready) {
		(void)gpio_pin_toggle_dt(&rx_led);
	}
#endif

	/* On-target gap/duplicate accounting: survives host-side serial loss.
	 * Testbed has a single sender, so any jump > 1 is air loss and any
	 * repeat/backwards step is a duplicate. Wraps handled via uint16
	 * arithmetic: a forward jump of 1 is normal, anything else forward
	 * counts (delta - 1) missing; equal/backwards counts one duplicate.
	 */
	if (!link_has_seq) {
		link_has_seq = true;
	} else {
		uint16_t delta = (uint16_t)(r.seq - link_last_seq);

		if (delta == 0 || delta > 0x8000U) {
			link_dups++;
		} else if (delta > 1) {
			link_gaps += (uint32_t)(delta - 1U);
		}
	}
	link_last_seq = r.seq;

	{
		uint32_t ok = (uint32_t)atomic_get(&rx_ok);

		if (ok == 1) {
			link_rssi_min = link_rssi_max = (int16_t)rssi;
			link_lqi_min = link_lqi_max = lqi;
		} else {
			if (rssi < link_rssi_min) {
				link_rssi_min = (int16_t)rssi;
			}
			if (rssi > link_rssi_max) {
				link_rssi_max = (int16_t)rssi;
			}
			if (lqi < link_lqi_min) {
				link_lqi_min = lqi;
			}
			if (lqi > link_lqi_max) {
				link_lqi_max = lqi;
			}
		}
		link_rssi_sum += (int32_t)rssi;
		link_lqi_sum += lqi;
	}

#if defined(CONFIG_RADIO_TESTBED_FRAME_LOG)
	/* Full-field line: every payload byte the TX side emits is echoed
	 * here, so the host monitor can verify the complete codec round-trip
	 * (accel + flags included) per sequence. `rx_uptime` is the
	 * receiver's clock for air-time bounds. */
	LOG_INF("rx node=%u seq=%u light=%u temp_c_x100=%d "
		"accel=%d,%d,%d uptime=%u flags=%u "
		"rssi=%d lqi=%u rx_uptime=%u ok=%u",
		r.node_id, r.seq, r.light, r.temp_c_x100,
		r.accel[0], r.accel[1], r.accel[2],
		r.uptime_ms, r.flags,
		rssi, lqi, k_uptime_get_32(),
		(uint32_t)atomic_get(&rx_ok));
#endif

	net_pkt_unref(pkt);
	return 0;
}

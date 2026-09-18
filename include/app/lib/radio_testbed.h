/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_RADIO_TESTBED_H_
#define APP_LIB_RADIO_TESTBED_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup lib_radio_testbed Raw-mode shared receive path
 * @ingroup lib
 * @{
 *
 * @brief Single `net_recv_data()` for the raw-mode testbed images.
 *
 * `CONFIG_IEEE802154_RAW_MODE` shrinks the net stack to `net_pkt.c` only, so
 * the stack provides no `net_recv_data()` and the rf2xx driver needs the
 * application to supply it (same shape as `samples/net/wpan_serial`). This
 * file provides it once, shared by `radio-tx-testbed` (which only needs the
 * symbol to link) and `radio-rx-testbed` (which verifies the link with it).
 *
 * Every received frame is classified:
 *  - **ok**: a decodable sensor frame that also matches the configured
 *    filter (PAN and/or source address);
 *  - **foreign**: a well-formed 802.15.4 data frame that is not ours (wrong
 *    PAN, wrong source, or a non-sensor payload length) — this is the
 *    neighbour's traffic and must not pollute link statistics;
 *  - **malformed**: not a supported data frame at all (corrupt/truncated,
 *    beacon, security-enabled, ...).
 *
 * Only `ok` frames feed the link statistics and toggle LED0. Every event is
 * appended to the on-target ring buffer (when enabled) with its RSSI/LQI so
 * the host can pull a lossless per-frame record set on demand.
 */

#include <app/lib/sensor_frame.h>

/** @brief Classification of one received frame. */
enum radio_testbed_rx_status {
	RADIO_TESTBED_RX_OK = 0,        /**< valid sensor frame, filter match */
	RADIO_TESTBED_RX_FOREIGN = 1,   /**< valid 802.15.4 frame, not ours */
	RADIO_TESTBED_RX_MALFORMED = 2, /**< not a supported 802.15.4 data frame */
};

/**
 * @brief Which frames count as ours.
 *
 * Left all-zero (the default) nothing is filtered: every decodable sensor
 * frame counts as `ok`, which preserves the historical promiscuous testbed
 * behaviour. `radio-rx-testbed` configures it at boot so neighbour traffic is
 * reported as foreign instead of inflating `dropped`.
 */
struct radio_testbed_filter {
	bool     check_pan;        /**< require dst/src PAN == pan_id */
	uint16_t pan_id;           /**< expected PAN identifier */
	bool     check_src;        /**< require a short source == src_short_addr */
	uint16_t src_short_addr;   /**< expected sender short address */
};

/**
 * @brief Configure the RX activity LED (`led0` alias).
 *
 * Fail-open: returns < 0 when the alias is missing or the GPIO is not
 * ready, in which case reception and logging still work, just without the
 * blink. Called once at boot; `net_recv_data()` also retries lazily on the
 * first frame so a forgotten call cannot wedge the LED off forever.
 *
 * @retval 0 LED ready.
 * @retval -ENODEV no `led0` alias or GPIO not ready.
 */
int radio_testbed_led_init(void);

/**
 * @brief Snapshot the receive counters.
 *
 * @param ok      set to the number of valid sensor frames seen (may be NULL).
 * @param dropped set to the number of rejected frames, i.e. foreign plus
 *                malformed (may be NULL).
 */
void radio_testbed_stats(uint32_t *ok, uint32_t *dropped);

/**
 * @brief On-target link statistics (loss-resilient, no serial needed).
 *
 * Updated in the rf2xx RX thread, polled by the app's stats line. Survives
 * dropped/scrolled host log lines: gap/duplicate counts and RSSI/LQI
 * extremes are kept on the target itself. Single-sender testbed: sequence
 * gaps are measured against the last valid app `seq` seen.
 */
struct radio_testbed_link_stats {
	uint32_t ok;        /**< valid, filter-matching sensor frames */
	uint32_t dropped;   /**< foreign + malformed */
	uint32_t foreign;   /**< well-formed 802.15.4 frames that are not ours */
	uint32_t malformed; /**< frames that are not valid data frames at all */
	uint32_t gaps;      /**< app-seq jumps > 1 (lost frames on air) */
	uint32_t dups;      /**< app-seq repeats or backwards steps */
	uint16_t last_seq;  /**< app `seq` of the most recent valid frame */
	bool has_seq;       /**< false until the first valid frame */
	int16_t rssi_min;   /**< dBm, valid when ok > 0 */
	int16_t rssi_max;   /**< dBm, valid when ok > 0 */
	int32_t rssi_sum;   /**< dBm sum, for host-side average */
	uint32_t lqi_min;   /**< valid when ok > 0 */
	uint32_t lqi_max;   /**< valid when ok > 0 */
	uint32_t lqi_sum;   /**< for host-side average */
};

/**
 * @brief Snapshot the extended link statistics (may be called any time).
 */
void radio_testbed_link_stats(struct radio_testbed_link_stats *out);

/**
 * @brief Set the "is this frame ours" filter (see struct docs).
 *
 * @param filter filter to copy, or NULL to clear back to accept-everything.
 */
void radio_testbed_set_filter(const struct radio_testbed_filter *filter);

/**
 * @brief Total frames appended to the ring since boot (wraps at 32 bits).
 *
 * This doubles as the global append index stamped on every record and printed
 * as `idx=` by radio_testbed_dump(), so records can be identified across dumps
 * even when the ring laps the reader. Compare successive values across dumps:
 * if the delta exceeds the number of records the dump returned, the oldest
 * records were overwritten before the host collected them.
 */
uint32_t radio_testbed_ring_total(void);

/**
 * @brief Number of records currently held by the ring.
 */
uint32_t radio_testbed_ring_count(void);

/**
 * @brief Clear the ring (does not touch the counters).
 */
void radio_testbed_ring_reset(void);

/**
 * @brief Reset the receive counters and link statistics (does not touch the
 *        ring).
 */
void radio_testbed_reset_stats(void);

/**
 * @brief Emit the ring contents as parseable log lines.
 *
 * Prints `BULK_BEGIN total=<n> count=<n>`, one `BULK ...` line per record in
 * arrival order (oldest first), and `BULK_END total=<n> count=<n>`. Each
 * record line starts with `idx=<n>`, its own global append index (the value
 * radio_testbed_ring_total() had at the time of that frame). The reader should
 * key on `idx` rather than on the record's position in the dump: reception
 * continues during a dump and can overwrite slots that have not been streamed
 * yet, and `idx` turns that into a detectable hole instead of a silent
 * misattribution. Intended to be called from a console command thread in
 * response to the host sending `dump`. Uses the logging subsystem so it cannot
 * interleave byte-wise with the RX thread's own log output.
 */
void radio_testbed_dump(void);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_RADIO_TESTBED_H_ */

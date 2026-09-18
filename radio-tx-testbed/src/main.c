/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * radio-tx-testbed: raw-mode 802.15.4 transmitter for samr21_xpro.
 *
 * Encodes a sensor reading with lib/radio/sensor_frame.c and hands the
 * 27-byte PSDU to the on-board AT86RF233 via the rf2xx radio API, once per
 * TX_PERIOD_MS (see lib/radio/TX-PLAN.md §4). LED0 toggles on every
 * successful transmission so traffic is visible without a log cable.
 *
 * Pair with radio-rx-testbed on a second SAM R21 board: same channel,
 * same PAN ID, then watch both LEDs blink in step.
 *
 * Raw mode leaves net_recv_data() undefined, so the receive side lives in
 * the shared lib (lib/radio/radio_testbed.c) that this image already links.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <zephyr/net/net_pkt.h>
#include <zephyr/net/ieee802154_radio.h>

#include <app/lib/sensor_frame.h>
#include <app/lib/radio_test_pattern.h>

#include <zephyr/app_version.h>

LOG_MODULE_REGISTER(radio_tx, CONFIG_RADIO_TX_TESTBED_LOG_LEVEL);

/* Must match the RX side (radio-rx-testbed / TX-PLAN.md §6). */
#define TX_CHANNEL   15
#define TX_PAN_ID    0xCAFE
#define TX_NODE_ID   1
#define TX_POWER_DBM 0
#define TX_PERIOD_MS 1000

static const struct device *const radio =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_ieee802154));
static const struct gpio_dt_spec led =
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static int radio_tx_send(const struct ieee802154_radio_api *api,
			 struct sensor_frame_cfg *cfg,
			 const struct sensor_reading *reading)
{
	uint8_t psdu[SENSOR_FRAME_LEN];
	struct net_pkt *pkt;
	struct net_buf *frag;
	int ret;

	ret = sensor_frame_encode(psdu, sizeof(psdu), cfg, reading);
	if (ret < 0) {
		return ret;
	}

	pkt = net_pkt_alloc_with_buffer(NULL, SENSOR_FRAME_LEN,
					NET_AF_UNSPEC, 0, K_NO_WAIT);
	if (pkt == NULL) {
		return -ENOMEM;
	}

	if (net_pkt_write(pkt, psdu, SENSOR_FRAME_LEN) != 0) {
		net_pkt_unref(pkt);
		return -ENOMEM;
	}

	frag = net_buf_frag_last(pkt->buffer);
	ret = api->tx(radio, IEEE802154_TX_MODE_CSMA_CA, pkt, frag);
	net_pkt_unref(pkt);

	return ret; /* 0 ok; -EBUSY / -EAGAIN -> backoff and retry */
}

int main(void)
{
	const struct ieee802154_radio_api *api;
	struct sensor_frame_cfg cfg = {
		.pan_id = TX_PAN_ID,
		.dst_short_addr = 0xFFFF, /* broadcast in v1 */
		.src_short_addr = TX_NODE_ID,
		.ack_request = false,
	};
	uint16_t app_seq = 0;
	uint32_t tx_ok = 0, tx_busy = 0, tx_fail = 0;
	int ret;

	printk("Radio TX testbed %s (samr21_xpro, ch %d, pan 0x%04x, node %d, "
	       "pwr %d dBm, period %d ms)\n",
	       APP_VERSION_STRING, TX_CHANNEL, TX_PAN_ID, TX_NODE_ID,
	       TX_POWER_DBM, TX_PERIOD_MS);

	if (!gpio_is_ready_dt(&led)) {
		LOG_WRN("LED not ready, continuing without blink");
	} else {
		ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			LOG_WRN("LED configure failed (%d)", ret);
		}
	}

	if (!device_is_ready(radio)) {
		LOG_ERR("Radio not ready");
		return 0;
	}

	api = (const struct ieee802154_radio_api *)radio->api;
	ret = api->set_channel(radio, TX_CHANNEL);
	if (ret < 0) {
		LOG_ERR("set_channel(%d) failed (%d)", TX_CHANNEL, ret);
	}
	ret = api->set_txpower(radio, TX_POWER_DBM);
	if (ret < 0) {
		LOG_ERR("set_txpower(%d) failed (%d)", TX_POWER_DBM, ret);
	}

	ret = api->start(radio);
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("Radio start failed (%d)", ret);
		return 0;
	}

	LOG_INF("Transmitting 1 frame / %d ms", TX_PERIOD_MS);

	while (1) {
		struct sensor_reading r;

		/* Deterministic, seq-keyed payload: the monitor recomputes the
		 * same values from the sequence number and checks the received
		 * fields against them, so a stuck/mis-mapped field cannot pass.
		 * uptime_ms carries the real sender clock.
		 */
		radio_test_pattern_fill(app_seq, TX_NODE_ID, &r);
		r.uptime_ms = k_uptime_get_32();
		app_seq++;

		cfg.mac_seq++;

		ret = radio_tx_send(api, &cfg, &r);
		if (ret == 0) {
			tx_ok++;
			gpio_pin_toggle_dt(&led);
			LOG_INF("tx node=%u seq=%u light=%u temp_c_x100=%d "
				"accel=%d,%d,%d uptime=%u flags=%u mac_seq=%u ok",
				r.node_id, r.seq, r.light, r.temp_c_x100,
				r.accel[0], r.accel[1], r.accel[2],
				r.uptime_ms, r.flags, cfg.mac_seq);
		} else if (ret == -EBUSY || ret == -EAGAIN) {
			tx_busy++;
			LOG_WRN("tx seq=%u busy (%d), will retry next period",
				r.seq, ret);
		} else {
			tx_fail++;
			LOG_WRN("tx seq=%u failed (%d)", r.seq, ret);
		}

		/* Periodic totals: one scrolled/lost per-packet line must not
		 * destroy the run's TX-side accounting. Busy rate here is the
		 * channel-congestion signal (try another channel 11-26). */
		if ((tx_ok + tx_busy + tx_fail) % 10U == 0U) {
			LOG_INF("tx_stats ok=%u busy=%u fail=%u", tx_ok, tx_busy,
				tx_fail);
		}

		k_sleep(K_MSEC(TX_PERIOD_MS));
	}

	return 0;
}

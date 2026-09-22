/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Gateway bring-up image for samr21_xpro.
 *
 * Raw-mode 802.15.4 receiver: the radio is brought up promiscuous on
 * channel 15 with our PAN ID (0xCAFE) set in the hardware filter, and every
 * received frame is handed to net_recv_data() below (raw mode leaves that
 * symbol undefined, so this file provides it). Frames whose MAC payload is
 * exactly the 18-byte sensor payload decode via sensor_frame_decode() and
 * are printed to the console; anything else is just counted.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net_buf.h>
#include <zephyr/net/ieee802154_radio.h>

#include <app/lib/sensor_frame.h>

#include <zephyr/app_version.h>

LOG_MODULE_REGISTER(gateway, CONFIG_GATEWAY_LOG_LEVEL);

#define GW_CHANNEL 15
#define GW_PAN_ID  0xCAFE

static const struct device *const radio =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_ieee802154));

static uint32_t rx_ok;
static uint32_t rx_other;

/*
 * Called by the rf2xx driver RX thread for every received frame. Runs in
 * driver context: decode fast, never block, always unref the pkt.
 */
int net_recv_data(struct net_if *iface, struct net_pkt *pkt)
{
	struct sensor_reading r;
	struct net_buf *frag;

	ARG_UNUSED(iface);

	if (pkt == NULL) {
		return -EINVAL;
	}

	frag = net_buf_frag_last(pkt->buffer);
	if (frag != NULL && sensor_frame_decode(frag->data, frag->len, &r) == 0) {
		rx_ok++;
		printk("rx node=%u seq=%u light=%u temp_c_x100=%d "
		       "accel=%d,%d,%d uptime=%u flags=%u\n",
		       r.node_id, r.seq, r.light, r.temp_c_x100,
		       r.accel[0], r.accel[1], r.accel[2],
		       r.uptime_ms, r.flags);
	} else {
		rx_other++;
	}

	net_pkt_unref(pkt);
	return 0;
}

int main(void)
{
	const struct ieee802154_radio_api *api;
	int ret;

	printk("Gateway %s (samr21_xpro, ch %d, pan 0x%04x, promiscuous)\n",
	       APP_VERSION_STRING, GW_CHANNEL, GW_PAN_ID);

	if (!device_is_ready(radio)) {
		LOG_ERR("Radio not ready");
		return 0;
	}

	api = (const struct ieee802154_radio_api *)radio->api;

	ret = api->set_channel(radio, GW_CHANNEL);
	if (ret < 0) {
		LOG_ERR("set_channel(%d) failed (%d)", GW_CHANNEL, ret);
	}

	/* Our PAN ID, in the hardware filter. Promiscuous mode below accepts
	 * everything regardless for now; the ID is set so the filter is ready
	 * when we stop being promiscuous.
	 */
	{
		struct ieee802154_filter f = { .pan_id = GW_PAN_ID };

		ret = api->filter(radio, true, IEEE802154_FILTER_TYPE_PAN_ID, &f);
		if (ret < 0) {
			LOG_WRN("PAN ID filter refused (%d), continuing", ret);
		}
	}

	/* Promiscuous for now: accept any on-channel frame. */
	{
		struct ieee802154_config cfg = { .promiscuous = true };

		ret = api->configure(radio, IEEE802154_CONFIG_PROMISCUOUS, &cfg);
		if (ret < 0) {
			LOG_WRN("Promiscuous mode refused (%d), using default filter",
				ret);
		}
	}

	ret = api->start(radio);
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("Radio start failed (%d)", ret);
		return 0;
	}

	LOG_INF("Listening promiscuous on ch %d, PAN ID 0x%04x set", GW_CHANNEL, GW_PAN_ID);

	while (1) {
		k_sleep(K_MSEC(5000));
		LOG_INF("gw ok=%u other=%u", rx_ok, rx_other);
	}

	return 0;
}

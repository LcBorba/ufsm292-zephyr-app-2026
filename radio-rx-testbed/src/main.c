/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * radio-rx-testbed: raw-mode 802.15.4 receiver for samr21_xpro.
 *
 * The receive path lives in the shared lib (lib/radio/radio_testbed.c):
 * raw mode leaves net_recv_data() undefined, so the lib provides it. It
 * classifies each frame as ours / foreign / malformed, keeps the per-frame
 * data in an on-target ring buffer, and exposes link statistics. This image
 * brings the radio up (channel + filter), sets the expected network so
 * neighbour traffic is reported as *foreign* rather than link loss, and
 * serves a small console command set so the host can pull bulk data:
 *
 *   dump   -> print the ring contents as BULK/BULK_BEGIN/BULK_END log lines
 *   reset  -> clear counters and the ring
 *   stats  -> print the link statistics line now
 *   help   -> list commands
 *
 * Pair with radio-tx-testbed on a second SAM R21 board: same channel,
 * then watch both LEDs blink in step.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/console/console.h>

#include <zephyr/net/ieee802154_radio.h>

#include <app/lib/radio_testbed.h>

#include <zephyr/app_version.h>

LOG_MODULE_REGISTER(radio_rx, CONFIG_RADIO_RX_TESTBED_LOG_LEVEL);

/* Must match the TX side (radio-tx-testbed / TX-PLAN.md §6). */
#define RX_CHANNEL     15
#define RX_PAN_ID      0xCAFE
#define RX_SRC_NODE    1

static const struct device *const radio =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_ieee802154));

static void log_link_stats(void)
{
	struct radio_testbed_link_stats st;
	uint32_t ok;

	radio_testbed_link_stats(&st);
	ok = st.ok;

	/* Same parseable shape with and without frames. The monitor's LINK_RE
	 * keys have to be present *before* the first valid frame too, so that a
	 * silent link still yields a `target link` summary line -- and that is
	 * precisely the line that carries `malformed=N`, i.e. where an unstripped
	 * FCS or a codec mismatch shows up. Zeroed RSSI/LQI until ok > 0. */
	LOG_INF("link ok=%u dropped=%u foreign=%u malformed=%u "
		"gaps=%u dups=%u last_seq=%u "
		"rssi_min=%d rssi_max=%d rssi_avg=%d "
		"lqi_min=%u lqi_max=%u lqi_avg=%u",
		ok, st.dropped, st.foreign, st.malformed,
		st.gaps, st.dups,
		(unsigned int)(st.has_seq ? st.last_seq : 0U),
		ok > 0U ? st.rssi_min : 0, ok > 0U ? st.rssi_max : 0,
		ok > 0U ? (int)(st.rssi_sum / (int32_t)ok) : 0,
		ok > 0U ? st.lqi_min : 0U, ok > 0U ? st.lqi_max : 0U,
		ok > 0U ? (unsigned int)(st.lqi_sum / ok) : 0U);
}

/*
 * Console command thread. The host monitor sends `dump\n` over the same
 * serial port it reads logs from; console_getline() gives us a blocking line
 * reader without pulling in the full shell. The dump goes through LOG_INF so
 * it cannot interleave byte-wise with the RX thread's own log output.
 */
#define CMD_STACK_SIZE 1024
K_THREAD_STACK_DEFINE(cmd_stack, CMD_STACK_SIZE);
static struct k_thread cmd_thread_data;

static void cmd_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	console_getline_init();

	for (;;) {
		char *line = console_getline();

		if (line == NULL) {
			continue;
		}

		if (strcmp(line, "dump") == 0) {
			radio_testbed_dump();
		} else if (strcmp(line, "reset") == 0) {
			radio_testbed_reset_stats();
			radio_testbed_ring_reset();
			printk("reset ok\n");
		} else if (strcmp(line, "stats") == 0) {
			log_link_stats();
		} else if (strcmp(line, "help") == 0) {
			printk("commands: dump, reset, stats, help\n");
		} else if (line[0] != '\0') {
			printk("unknown command '%s' (try help)\n", line);
		}
	}
}

int main(void)
{
	const struct ieee802154_radio_api *api;
	struct radio_testbed_filter filter = {
		.check_pan = true,
		.pan_id = RX_PAN_ID,
		.check_src = true,
		.src_short_addr = RX_SRC_NODE,
	};
	int ret;

	printk("Radio RX testbed %s (samr21_xpro, ch %d, pan 0x%04x, src %d)\n",
	       APP_VERSION_STRING, RX_CHANNEL, RX_PAN_ID, RX_SRC_NODE);

	if (radio_testbed_led_init() < 0) {
		LOG_WRN("RX LED not ready, continuing without blink");
	}

	radio_testbed_set_filter(&filter);

	k_thread_create(&cmd_thread_data, cmd_stack,
			K_THREAD_STACK_SIZEOF(cmd_stack),
			cmd_thread, NULL, NULL, NULL,
			K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
	k_thread_name_set(&cmd_thread_data, "cmd");

	if (!device_is_ready(radio)) {
		LOG_ERR("Radio not ready");
		return 0;
	}

	api = (const struct ieee802154_radio_api *)radio->api;

	ret = api->set_channel(radio, RX_CHANNEL);
	if (ret < 0) {
		LOG_ERR("set_channel(%d) failed (%d)", RX_CHANNEL, ret);
	}

	/* Testbed: accept any on-channel frame, then classify it against the
	 * filter above. Fail open so a driver that rejects the configure call
	 * still receives with its default filter.
	 */
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

	LOG_INF("Listening, LED0 blinks per valid sensor frame; send 'dump' for bulk");

	while (1) {
		k_sleep(K_MSEC(5000));
		log_link_stats();
	}

	return 0;
}

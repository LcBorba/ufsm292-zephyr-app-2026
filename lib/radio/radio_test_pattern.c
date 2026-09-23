/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <app/lib/radio_test_pattern.h>

/*
 * Pure C, no Zephyr dependencies: used by sensor senders and pinned by the
 * host unit tests. See radio_test_pattern.h for the formula.
 */
void radio_test_pattern_fill(uint16_t seq, uint8_t node_id,
			     struct sensor_reading *reading)
{
	uint32_t s = (uint32_t)seq;

	if (reading == NULL) {
		return;
	}

	reading->node_id = node_id;
	reading->seq = seq;
	reading->light = (uint16_t)(s * 40503U + 12345U);
	reading->temp_c_x100 = (int16_t)(1800U + (s % 600U));
	reading->accel[0] = (int16_t)(uint16_t)(s * 3U + 40000U);
	reading->accel[1] = (int16_t)(uint16_t)(s * 5U + 30000U);
	reading->accel[2] = (int16_t)(uint16_t)(s * 7U + 20000U);
	reading->flags = (uint8_t)((s ^ (s >> 8)) & 0xFFU);
}

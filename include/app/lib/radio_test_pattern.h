/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_RADIO_TEST_PATTERN_H_
#define APP_LIB_RADIO_TEST_PATTERN_H_

#include <stdint.h>

#include <app/lib/sensor_frame.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup lib_radio_test_pattern Deterministic link-test payload
 * @ingroup lib
 * @{
 *
 * @brief Fill a sensor reading from a repeatable pattern keyed by sequence.
 *
 * The link testbed must not send a constant payload: a stuck or mis-mapped
 * field would look healthy. Both ends of the bench (the TX image and the
 * Python monitor in `test-radio-link.sh`) can reproduce this pattern from the
 * application `seq` alone, so the monitor verifies the received payload
 * against an *independently computed* expectation instead of trusting the TX
 * log. Keeping the formula here, unit-tested with golden vectors, makes the
 * C and Python copies a checked contract rather than folklore.
 *
 * The pattern is, with `s = (uint32_t)seq` and every narrow value taken
 * modulo 2^N then reinterpreted as two's complement for signed fields:
 *
 *   light       = (uint16_t)(s * 40503 + 12345)
 *   temp_c_x100 = 1800 + (s % 600)          (18.00 .. 23.99 C)
 *   accel[0]    = (int16_t)(s * 3 + 40000)
 *   accel[1]    = (int16_t)(s * 5 + 30000)
 *   accel[2]    = (int16_t)(s * 7 + 20000)
 *   flags       = (uint8_t)((s ^ (s >> 8)) & 0xFF)
 *   node_id     = node_id argument
 *   seq         = seq argument
 *
 * `uptime_ms` is deliberately **not** touched: it carries the sender's real
 * clock and cannot be predicted by the receiver.
 *
 * @param seq     Application sequence number (0..65535).
 * @param node_id Sensor node id to stamp.
 * @param reading Reading to fill (must not be NULL).
 */
void radio_test_pattern_fill(uint16_t seq, uint8_t node_id,
			     struct sensor_reading *reading);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_RADIO_TEST_PATTERN_H_ */

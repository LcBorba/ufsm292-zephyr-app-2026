/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_SENSOR_FRAME_H_
#define APP_LIB_SENSOR_FRAME_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup lib_sensor_frame Shared 802.15.4 sensor-frame codec
 * @ingroup lib
 * @{
 *
 * @brief Encode/decode the 18-byte sensor payload inside a proper 802.15.4
 *        MAC data frame.
 *
 * Shared by the sensor nodes (encode + transmit) and the gateway (receive +
 * decode), so the on-air format is defined in exactly one place. The codec is
 * pure C: it does not depend on Zephyr, the radio driver or the network stack,
 * which keeps it unit-testable on any host.
 */

/** Application payload length fixed by `gateway/AGENTS.md`. */
#define SENSOR_PAYLOAD_LEN 18U

/**
 * Length of the v1 MAC header emitted by the encoder:
 * FCF (2) + sequence number (1) + dst PAN (2) + dst short addr (2) +
 * src short addr (2) = 9 bytes. The source PAN is omitted (PAN ID compression).
 */
#define SENSOR_FRAME_HEADER_LEN 9U

/** Full v1 frame length handed to the radio (FCS is appended by hardware). */
#define SENSOR_FRAME_LEN (SENSOR_FRAME_HEADER_LEN + SENSOR_PAYLOAD_LEN)

/** Decoded 18-byte application payload, in host byte order. */
struct sensor_reading {
	uint8_t  node_id;
	uint16_t seq;
	uint16_t light;
	int16_t  temp_c_x100;
	int16_t  accel[3];
	uint32_t uptime_ms;
	uint8_t  flags;
};

/**
 * @brief MAC parameters used by the encoder and validated by the decoder.
 *
 * v1 profile: data frame, no security, short destination and source addresses,
 * PAN ID compression enabled. `src_short_addr` is conventionally set equal to
 * `node_id` so receivers/sniffers can filter on the MAC source.
 */
struct sensor_frame_cfg {
	uint16_t pan_id;          /**< Destination PAN identifier. */
	uint16_t dst_short_addr;  /**< Destination short address; 0xFFFF = broadcast. */
	uint16_t src_short_addr;  /**< Source (sensor) short address. */
	uint8_t  mac_seq;         /**< 802.15.4 sequence number (caller-owned). */
	bool     ack_request;     /**< Ack Request bit; false for broadcast in v1. */
};

/**
 * @brief Build one 802.15.4 data frame (MAC header + 18-byte payload).
 *
 * The sensor data is taken from the provided reading struct. The frame is
 * written little-endian, exactly as it goes on the air. The 2-byte FCS is
 * **not** appended: the rf2xx radio hardware adds it, and the receiver's
 * driver strips it before decode.
 *
 * @param buf    Destination buffer.
 * @param cap    Capacity of @p buf in bytes.
 * @param cfg    MAC addressing/sequence parameters (must not be NULL).
 * @param reading  Sensor reading to encode (must not be NULL).
 *
 * @retval >=0 Number of bytes written (`SENSOR_FRAME_LEN`, 27).
 * @retval -EINVAL @p buf, @p cfg, or @p reading is NULL.
 * @retval -ENOSPC @p cap is smaller than `SENSOR_FRAME_LEN`.
 */
int sensor_frame_encode(uint8_t *buf, size_t cap,
			const struct sensor_frame_cfg *cfg,
			const struct sensor_reading *reading);

/**
 * @brief Decode an 802.15.4 data frame carrying an 18-byte sensor payload.
 *
 * Accepts any data frame whose header can be sized from the Frame Control
 * field (short or extended addresses, PAN ID compression on or off) as long as
 * the MAC payload is exactly `SENSOR_PAYLOAD_LEN` bytes. Security-enabled
 * frames, non-data frames and reserved addressing modes are rejected.
 *
 * @p psdu must have the FCS already removed (the rf2xx driver does this when
 * `CONFIG_IEEE802154_L2_PKT_INCL_FCS` is not set, which is the case for the
 * custom L2).
 *
 * @param psdu Received PSDU (header + payload, no FCS).
 * @param len  Length of @p psdu in bytes.
 * @param out  Decoded payload, in host byte order (must not be NULL).
 *
 * @retval 0 Success.
 * @retval -EINVAL @p psdu or @p out is NULL, or the frame is malformed,
 *                 non-data, security-enabled, or not exactly 18 payload bytes.
 */
int sensor_frame_decode(const uint8_t *psdu, size_t len,
			struct sensor_reading *out);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_SENSOR_FRAME_H_ */

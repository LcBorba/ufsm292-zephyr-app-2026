/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <app/lib/sensor_frame.h>

/*
 * IEEE 802.15.4 Frame Control field bits (LITTLE-ENDIAN in the frame, i.e. the
 * value below is the host-order FCF). Kept local so the codec stays free of any
 * Zephyr dependency and can be unit-tested on a plain host.
 */
#define FCF_TYPE_MASK      0x0007U
#define FCF_TYPE_DATA      0x0001U
#define FCF_SECURITY       0x0008U
#define FCF_FRAME_PENDING  0x0010U
#define FCF_ACK_REQUEST    0x0020U
#define FCF_PAN_COMPRESS   0x0040U
#define FCF_VERSION_SHIFT  12U
#define FCF_VERSION_MASK   0x3000U
#define FCF_DST_MODE_SHIFT 10U
#define FCF_DST_MODE_MASK  0x0C00U
#define FCF_SRC_MODE_SHIFT 14U
#define FCF_SRC_MODE_MASK  0xC000U

#define ADDR_MODE_NONE     0U
#define ADDR_MODE_RESERVED 1U
#define ADDR_MODE_SHORT    2U
#define ADDR_MODE_EXTENDED 3U

#define FCF_VERSION_2006   1U
#define FCF_VERSION_2003   0U
#define FCF_VERSION_RESERVED 3U

/*
 * Byte-wise little-endian read/write helpers.
 *
 * We do *not* cast the buffer pointer to (uint16_t *) or (uint32_t *)
 * because:
 *  1) The buffer may be unaligned -- dereferencing a misaligned pointer is
 *     undefined behaviour in C and causes a HardFault on Cortex-M0+.
 *  2) The on-wire byte order (little-endian) must be explicit regardless of
 *     the host CPU's native endianness, so this code is portable.
 *  3) A byte-wise copy compiles to a handful of load/store instructions on
 *     any ARM target, with no risk of the compiler generating an unaligned
 *     access or depending on struct padding.
 */
static uint16_t rd_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void wr_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static uint32_t rd_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

int sensor_frame_encode(uint8_t *buf, size_t cap,
			const struct sensor_frame_cfg *cfg,
			const struct sensor_reading *reading)
{
	uint16_t fcf;
	uint8_t *p;

	if (buf == NULL || cfg == NULL || reading == NULL) {
		return -EINVAL;
	}

	if (cap < SENSOR_FRAME_LEN) {
		return -ENOSPC;
	}

	fcf = FCF_TYPE_DATA |
	      (FCF_VERSION_2006 << FCF_VERSION_SHIFT) |
	      FCF_PAN_COMPRESS |
	      (ADDR_MODE_SHORT << FCF_DST_MODE_SHIFT) |
	      (ADDR_MODE_SHORT << FCF_SRC_MODE_SHIFT);

	if (cfg->ack_request) {
		fcf |= FCF_ACK_REQUEST;
	}

	/* Mandatory header: FCF, sequence number, dst PAN, dst addr, src addr. */
	wr_le16(&buf[0], fcf);
	buf[2] = cfg->mac_seq;
	wr_le16(&buf[3], cfg->pan_id);
	wr_le16(&buf[5], cfg->dst_short_addr);
	wr_le16(&buf[7], cfg->src_short_addr);

	/* 18-byte application payload, little-endian, byte-wise (no unaligned
	 * word accesses: the target is a Cortex-M0+).
	 */
	p = &buf[SENSOR_FRAME_HEADER_LEN];
	p[0] = reading->node_id;
	wr_le16(&p[1], reading->seq);
	wr_le16(&p[3], reading->light);
	wr_le16(&p[5], (uint16_t)reading->temp_c_x100);
	wr_le16(&p[7], (uint16_t)reading->accel[0]);
	wr_le16(&p[9], (uint16_t)reading->accel[1]);
	wr_le16(&p[11], (uint16_t)reading->accel[2]);
	wr_le32(&p[13], reading->uptime_ms);
	p[17] = reading->flags;

	return (int)SENSOR_FRAME_LEN;
}

/**
 * Compute the MAC header length of a data frame, or return a negative errno.
 * Handles both short (2-byte) and extended (8-byte) addresses and PAN ID
 * compression, so the decoder is not limited to the exact v1 encoder profile.
 */
static int mac_header_len(const uint8_t *psdu, size_t len, size_t *hdr_len)
{
	uint16_t fcf;
	uint8_t version, dst_mode, src_mode;
	bool pan_comp;
	size_t hdr;

	if (len < 3U) {
		return -EINVAL;
	}

	fcf = rd_le16(psdu);

	if ((fcf & FCF_TYPE_MASK) != FCF_TYPE_DATA) {
		return -EINVAL;
	}

	if ((fcf & FCF_SECURITY) != 0U) {
		/* No AES in v1; an aux security header would change the layout. */
		return -EINVAL;
	}

	version = (uint8_t)((fcf & FCF_VERSION_MASK) >> FCF_VERSION_SHIFT);
	if (version == FCF_VERSION_RESERVED) {
		return -EINVAL;
	}

	dst_mode = (uint8_t)((fcf & FCF_DST_MODE_MASK) >> FCF_DST_MODE_SHIFT);
	src_mode = (uint8_t)((fcf & FCF_SRC_MODE_MASK) >> FCF_SRC_MODE_SHIFT);

	if (dst_mode == ADDR_MODE_RESERVED || src_mode == ADDR_MODE_RESERVED) {
		return -EINVAL;
	}

	pan_comp = (fcf & FCF_PAN_COMPRESS) != 0U;

	hdr = 2U /* FCF */ + 1U /* sequence number */;

	if (dst_mode != ADDR_MODE_NONE) {
		hdr += 2U; /* destination PAN */
		hdr += (dst_mode == ADDR_MODE_SHORT) ? 2U : 8U;
	}

	if (src_mode != ADDR_MODE_NONE) {
		/* Source PAN is present unless it is compressed away together
		 * with a present destination address.
		 */
		if (!(pan_comp && dst_mode != ADDR_MODE_NONE)) {
			hdr += 2U;
		}
		hdr += (src_mode == ADDR_MODE_SHORT) ? 2U : 8U;
	}

	*hdr_len = hdr;

	return 0;
}

int sensor_frame_decode(const uint8_t *psdu, size_t len,
			struct sensor_reading *out)
{
	size_t hdr;
	const uint8_t *p;
	int ret;

	if (psdu == NULL || out == NULL) {
		return -EINVAL;
	}

	ret = mac_header_len(psdu, len, &hdr);
	if (ret < 0) {
		return ret;
	}

	/* !SENSOR_PAYLOAD_LEN means either a foreign/short frame or an FCS that
	 * was not stripped; drop it either way.
	 */
	if (len != hdr + SENSOR_PAYLOAD_LEN) {
		return -EINVAL;
	}

	p = &psdu[hdr];

	out->node_id = p[0];
	out->seq = rd_le16(&p[1]);
	out->light = rd_le16(&p[3]);
	out->temp_c_x100 = (int16_t)rd_le16(&p[5]);
	out->accel[0] = (int16_t)rd_le16(&p[7]);
	out->accel[1] = (int16_t)rd_le16(&p[9]);
	out->accel[2] = (int16_t)rd_le16(&p[11]);
	out->uptime_ms = rd_le32(&p[13]);
	out->flags = p[17];

	return 0;
}

/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/sys/byteorder.h>

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
 * Byte-wise little-endian accessors from <zephyr/sys/byteorder.h>:
 * sys_get_le16()/sys_get_le32() and sys_put_le16()/sys_put_le32().
 *
 * These are the buffer-level accessors. Do not swap in sys_cpu_to_le16() /
 * sys_le16_to_cpu(): those are host-endianness value macros (a no-op on a
 * little-endian target) that say nothing about storage, and pairing them with
 * a (uint16_t *) cast of the buffer is undefined behaviour on an unaligned
 * address. sys_*_le* are byte-wise, so they are alignment-safe on the
 * Cortex-M0+ and always emit/parse the on-wire little-endian order.
 */

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
	sys_put_le16(fcf, &buf[0]);
	buf[2] = cfg->mac_seq;
	sys_put_le16(cfg->pan_id, &buf[3]);
	sys_put_le16(cfg->dst_short_addr, &buf[5]);
	sys_put_le16(cfg->src_short_addr, &buf[7]);

	/* 18-byte application payload, little-endian (sys_put_le* is byte-wise,
	 * so no unaligned word accesses: the target is a Cortex-M0+).
	 */
	p = &buf[SENSOR_FRAME_HEADER_LEN];
	p[0] = reading->node_id;
	sys_put_le16(reading->seq, &p[1]);
	sys_put_le16(reading->light, &p[3]);
	sys_put_le16((uint16_t)reading->temp_c_x100, &p[5]);
	sys_put_le16((uint16_t)reading->accel[0], &p[7]);
	sys_put_le16((uint16_t)reading->accel[1], &p[9]);
	sys_put_le16((uint16_t)reading->accel[2], &p[11]);
	sys_put_le32(reading->uptime_ms, &p[13]);
	p[17] = reading->flags;

	return (int)SENSOR_FRAME_LEN;
}

/* Address width for a present (non-NONE, non-RESERVED) addressing mode. */
static size_t addr_len(uint8_t mode)
{
	return (mode == ADDR_MODE_SHORT) ? 2U : 8U;
}

/**
 * Parse a data frame's MAC header into @p meta. Handles both short (2-byte)
 * and extended (8-byte) addresses and PAN ID compression, so callers see the
 * same header shape the decoder accepts. Returns 0 only when the header is a
 * supported 802.15.4 data frame and is fully present in @p len bytes.
 */
static int parse_header(const uint8_t *psdu, size_t len,
			struct sensor_frame_meta *meta)
{
	struct sensor_frame_meta m = { 0 };
	uint16_t fcf;
	uint8_t version, dst_mode, src_mode;
	bool pan_comp;
	size_t off;

	if (psdu == NULL || meta == NULL || len < 3U) {
		return -EINVAL;
	}

	fcf = sys_get_le16(psdu);

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

	m.fcf = fcf;
	m.mac_seq = psdu[2];
	m.dst_mode = dst_mode;
	m.src_mode = src_mode;
	m.pan_compressed = pan_comp;

	off = 2U /* FCF */ + 1U /* sequence number */;

	if (dst_mode != ADDR_MODE_NONE) {
		size_t alen = addr_len(dst_mode);

		if (len < off + 2U + alen) {
			return -EINVAL;
		}
		m.dst_pan_id = sys_get_le16(&psdu[off]);
		m.has_dst_pan = true;
		off += 2U;

		if (dst_mode == ADDR_MODE_SHORT) {
			m.dst_short_addr = sys_get_le16(&psdu[off]);
			m.dst_is_short = true;
		}
		off += alen;
	}

	if (src_mode != ADDR_MODE_NONE) {
		size_t alen = addr_len(src_mode);
		size_t need = alen;

		/* Source PAN is present unless it is compressed away together
		 * with a present destination address.
		 */
		if (!(pan_comp && dst_mode != ADDR_MODE_NONE)) {
			need += 2U;
		}
		if (len < off + need) {
			return -EINVAL;
		}
		if (need != alen) {
			m.src_pan_id = sys_get_le16(&psdu[off]);
			m.has_src_pan = true;
			off += 2U;
		}

		if (src_mode == ADDR_MODE_SHORT) {
			m.src_short_addr = sys_get_le16(&psdu[off]);
			m.src_is_short = true;
		}
		off += alen;
	}

	m.payload_off = off;
	m.payload_len = len - off;
	*meta = m;

	return 0;
}

int sensor_frame_parse_header(const uint8_t *psdu, size_t len,
			      struct sensor_frame_meta *meta)
{
	return parse_header(psdu, len, meta);
}

int sensor_frame_decode_meta(const uint8_t *psdu, size_t len,
			     struct sensor_reading *out,
			     struct sensor_frame_meta *meta)
{
	struct sensor_frame_meta m;
	const uint8_t *p;
	int ret;

	if (psdu == NULL || out == NULL) {
		return -EINVAL;
	}

	ret = parse_header(psdu, len, &m);
	if (ret < 0) {
		return ret;
	}

	/* !SENSOR_PAYLOAD_LEN means either a foreign/short frame or an FCS that
	 * was not stripped; drop it either way.
	 */
	if (m.payload_len != SENSOR_PAYLOAD_LEN) {
		return -EINVAL;
	}

	if (meta != NULL) {
		*meta = m;
	}

	p = &psdu[m.payload_off];

	out->node_id = p[0];
	out->seq = sys_get_le16(&p[1]);
	out->light = sys_get_le16(&p[3]);
	out->temp_c_x100 = (int16_t)sys_get_le16(&p[5]);
	out->accel[0] = (int16_t)sys_get_le16(&p[7]);
	out->accel[1] = (int16_t)sys_get_le16(&p[9]);
	out->accel[2] = (int16_t)sys_get_le16(&p[11]);
	out->uptime_ms = sys_get_le32(&p[13]);
	out->flags = p[17];

	return 0;
}

int sensor_frame_decode(const uint8_t *psdu, size_t len,
			struct sensor_reading *out)
{
	return sensor_frame_decode_meta(psdu, len, out, NULL);
}

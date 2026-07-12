/*
 * Wire protocol constants shared with the iOS companion app.
 * See docs/PROTOCOL.md for the full specification.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#define OBSC_MAGIC 0x4F425343u /* "OBSC" */
#define OBSC_PROTOCOL_VERSION 1
#define OBSC_HEADER_SIZE 20
#define OBSC_MAX_PAYLOAD (16u * 1024u * 1024u)

/* The app listens on this port on the device; the plugin dials it over
 * the LAN or through usbmuxd (USB). */
#define OBSC_USB_PORT 9979

enum obsc_packet_type {
	OBSC_PKT_HELLO = 1,
	OBSC_PKT_VIDEO_CONFIG = 2,
	OBSC_PKT_VIDEO = 3,
	OBSC_PKT_PING = 4,
	/* Clock sync for latency measurement: plugin sends REQ with its
	 * clock in pts; app echoes it in the RESP payload with its own
	 * clock in pts. */
	OBSC_PKT_TIMESYNC_REQ = 5,
	OBSC_PKT_TIMESYNC_RESP = 6,
	/* Camera remote control, plugin → app. UTF-8 JSON payload, e.g.
	 * {"cmd":"zoom","value":2.0} — see docs/PROTOCOL.md. */
	OBSC_PKT_CONTROL = 7,
	/* Camera state report, app → plugin. UTF-8 JSON snapshot of the
	 * current control values; keeps remote UIs in sync. */
	OBSC_PKT_STATE = 8,
};

#define OBSC_FLAG_KEYFRAME 0x0001

struct obsc_header {
	uint8_t version;
	uint8_t type;
	uint16_t flags;
	uint64_t pts_ns;
	uint32_t payload_size;
};

static inline uint16_t obsc_read_u16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] << 8 | (uint16_t)p[1]);
}

static inline uint32_t obsc_read_u32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static inline uint64_t obsc_read_u64(const uint8_t *p)
{
	return (uint64_t)obsc_read_u32(p) << 32 | obsc_read_u32(p + 4);
}

static inline void obsc_write_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static inline void obsc_write_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static inline void obsc_write_u64(uint8_t *p, uint64_t v)
{
	obsc_write_u32(p, (uint32_t)(v >> 32));
	obsc_write_u32(p + 4, (uint32_t)v);
}

static inline void obsc_build_header(uint8_t *buf, uint8_t type,
				     uint16_t flags, uint64_t pts_ns,
				     uint32_t payload_size)
{
	buf[0] = 'O';
	buf[1] = 'B';
	buf[2] = 'S';
	buf[3] = 'C';
	buf[4] = OBSC_PROTOCOL_VERSION;
	buf[5] = type;
	obsc_write_u16(buf + 6, flags);
	obsc_write_u64(buf + 8, pts_ns);
	obsc_write_u32(buf + 16, payload_size);
}

/* Parses a 20-byte header. Returns false on bad magic/version/size. */
static inline bool obsc_parse_header(const uint8_t *buf,
				     struct obsc_header *hdr)
{
	if (obsc_read_u32(buf) != OBSC_MAGIC)
		return false;

	hdr->version = buf[4];
	hdr->type = buf[5];
	hdr->flags = obsc_read_u16(buf + 6);
	hdr->pts_ns = obsc_read_u64(buf + 8);
	hdr->payload_size = obsc_read_u32(buf + 16);

	if (hdr->version != OBSC_PROTOCOL_VERSION)
		return false;
	if (hdr->payload_size > OBSC_MAX_PAYLOAD)
		return false;

	return true;
}

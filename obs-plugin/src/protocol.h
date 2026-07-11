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

#define OBSC_DEFAULT_PORT 9977
#define OBSC_DISCOVERY_PORT_OFFSET 1

#define OBSC_DISCOVER_REQUEST "OBSC_DISCOVER"
#define OBSC_DISCOVER_REPLY_PREFIX "OBSC_HERE:"

enum obsc_packet_type {
	OBSC_PKT_HELLO = 1,
	OBSC_PKT_VIDEO_CONFIG = 2,
	OBSC_PKT_VIDEO = 3,
	OBSC_PKT_PING = 4,
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

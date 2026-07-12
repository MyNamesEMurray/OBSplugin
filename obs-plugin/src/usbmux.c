#include <obs-module.h>
#include <util/bmem.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "usbmux.h"

#ifndef _WIN32
#include <sys/un.h>
#endif

/* usbmuxd binary protocol: 16-byte little-endian header + XML plist body. */
#define USBMUX_VERSION_PLIST 1
#define USBMUX_MSG_PLIST 8
#define USBMUX_MAX_REPLY (1024 * 1024)

#define PLIST_PROLOGUE \
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" \
	"<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" " \
	"\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"

static const char list_devices_xml[] =
	PLIST_PROLOGUE
	"<plist version=\"1.0\"><dict>"
	"<key>MessageType</key><string>ListDevices</string>"
	"<key>ProgName</key><string>obs-ios-camera</string>"
	"<key>ClientVersionString</key><string>1.0</string>"
	"</dict></plist>";

static const char connect_xml_fmt[] =
	PLIST_PROLOGUE
	"<plist version=\"1.0\"><dict>"
	"<key>MessageType</key><string>Connect</string>"
	"<key>DeviceID</key><integer>%ld</integer>"
	"<key>PortNumber</key><integer>%u</integer>"
	"<key>ProgName</key><string>obs-ios-camera</string>"
	"<key>ClientVersionString</key><string>1.0</string>"
	"</dict></plist>";

static void put_u32le(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)((v >> 8) & 0xff);
	p[2] = (uint8_t)((v >> 16) & 0xff);
	p[3] = (uint8_t)((v >> 24) & 0xff);
}

static uint32_t get_u32le(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static bool write_all(socket_t s, const void *buf, size_t len)
{
	const char *p = buf;
	while (len > 0) {
		int n = (int)send(s, p, (int)len, 0);
		if (n <= 0)
			return false;
		p += n;
		len -= (size_t)n;
	}
	return true;
}

static bool read_all(socket_t s, void *buf, size_t len)
{
	char *p = buf;
	while (len > 0) {
		int n = (int)recv(s, p, (int)len, 0);
		if (n <= 0)
			return false;
		p += n;
		len -= (size_t)n;
	}
	return true;
}

static void set_timeouts(socket_t s, int seconds)
{
#ifdef _WIN32
	DWORD ms = (DWORD)seconds * 1000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof(ms));
#else
	struct timeval tv = {.tv_sec = seconds, .tv_usec = 0};
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

static socket_t usbmuxd_open(void)
{
#ifdef _WIN32
	/* Apple Mobile Device Service (installed with iTunes) speaks the
	 * usbmuxd protocol on localhost:27015. */
	socket_t s = socket(AF_INET, SOCK_STREAM, 0);
	if (s == OBSC_INVALID_SOCKET)
		return s;

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(27015);

	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		net_close(s);
		return OBSC_INVALID_SOCKET;
	}
#else
	socket_t s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s == OBSC_INVALID_SOCKET)
		return s;

	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, "/var/run/usbmuxd", sizeof(addr.sun_path) - 1);

	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		net_close(s);
		return OBSC_INVALID_SOCKET;
	}
#endif
	set_timeouts(s, 3);
	return s;
}

static bool send_plist(socket_t s, uint32_t tag, const char *xml)
{
	uint32_t len = (uint32_t)strlen(xml);
	uint8_t hdr[16];
	put_u32le(hdr, len + 16);
	put_u32le(hdr + 4, USBMUX_VERSION_PLIST);
	put_u32le(hdr + 8, USBMUX_MSG_PLIST);
	put_u32le(hdr + 12, tag);
	return write_all(s, hdr, sizeof(hdr)) && write_all(s, xml, len);
}

/* Caller frees with bfree(). */
static char *recv_plist(socket_t s)
{
	uint8_t hdr[16];
	if (!read_all(s, hdr, sizeof(hdr)))
		return NULL;

	uint32_t total = get_u32le(hdr);
	if (total < 16 || total > USBMUX_MAX_REPLY)
		return NULL;

	size_t len = total - 16;
	char *buf = bmalloc(len + 1);
	if (len && !read_all(s, buf, len)) {
		bfree(buf);
		return NULL;
	}
	buf[len] = 0;
	return buf;
}

/* Best-effort: value of the first <integer> after "<key>name</key>". */
static bool find_int_after(const char *xml, const char *name, long *out)
{
	char pattern[64];
	snprintf(pattern, sizeof(pattern), "<key>%s</key>", name);

	const char *p = strstr(xml, pattern);
	if (!p)
		return false;
	p = strstr(p + strlen(pattern), "<integer>");
	if (!p)
		return false;

	*out = strtol(p + strlen("<integer>"), NULL, 10);
	return true;
}

/* Value of the last "<key>DeviceID</key><integer>" occurring before `limit`
 * (a byte offset into xml). In a device entry the top-level DeviceID
 * precedes the Properties block, so this pairs a ConnectionType with its
 * device. */
static bool find_device_id_before(const char *xml, size_t limit, long *out)
{
	const char *pat = "<key>DeviceID</key>";
	size_t plen = strlen(pat);
	const char *best = NULL;
	const char *p = xml;
	while ((p = strstr(p, pat)) != NULL) {
		if ((size_t)(p - xml) >= limit)
			break;
		best = p;
		p += plen;
	}
	if (!best)
		return false;
	const char *i = strstr(best + plen, "<integer>");
	if (!i)
		return false;
	*out = strtol(i + strlen("<integer>"), NULL, 10);
	return true;
}

/* Copies the string value of the first "<key>KEY</key><string>...".
 * following `from`. */
static bool find_string_after(const char *from, const char *key, char *out,
			      size_t out_size)
{
	char pattern[64];
	snprintf(pattern, sizeof(pattern), "<key>%s</key><string>", key);
	const char *p = strstr(from, pattern);
	if (!p)
		return false;
	p += strlen(pattern);
	const char *end = strchr(p, '<');
	if (!end)
		return false;
	size_t n = (size_t)(end - p);
	if (n >= out_size)
		n = out_size - 1;
	memcpy(out, p, n);
	out[n] = 0;
	return true;
}

int usbmux_list_devices(struct usbmux_device *out, int max)
{
	socket_t s = usbmuxd_open();
	if (s == OBSC_INVALID_SOCKET)
		return 0;

	if (!send_plist(s, 1, list_devices_xml)) {
		net_close(s);
		return 0;
	}
	char *reply = recv_plist(s);
	net_close(s);
	if (!reply)
		return 0;

	int count = 0;
	const char *pat = "<key>ConnectionType</key><string>";
	const char *p = reply;
	while (count < max && (p = strstr(p, pat)) != NULL) {
		const char *val = p + strlen(pat);
		bool is_usb = strncmp(val, "USB", 3) == 0;
		size_t pos = (size_t)(p - reply);
		p = val;
		if (!is_usb)
			continue;

		long id = -1;
		if (!find_device_id_before(reply, pos, &id) || id < 0)
			continue;

		char udid[64] = {0};
		/* SerialNumber lives in the same Properties dict, after
		 * ConnectionType. */
		find_string_after(val, "SerialNumber", udid, sizeof(udid));

		bool dup = false;
		for (int i = 0; i < count; i++)
			if (out[i].id == id)
				dup = true;
		if (dup)
			continue;

		out[count].id = id;
		snprintf(out[count].udid, sizeof(out[count].udid), "%s", udid);
		count++;
	}

	bfree(reply);
	return count;
}

socket_t usbmux_connect_device(long device_id, uint16_t device_port)
{
	char *reply = NULL;

	socket_t s = usbmuxd_open();
	if (s == OBSC_INVALID_SOCKET)
		return s;

	/* Ask the mux to connect us to the app's port on that device.
	 * PortNumber is expected in network byte order. */
	unsigned port_be = (unsigned)(((device_port & 0xff) << 8) |
				      ((device_port >> 8) & 0xff));
	char connect_xml[768];
	snprintf(connect_xml, sizeof(connect_xml), connect_xml_fmt, device_id,
		 port_be);

	if (!send_plist(s, 2, connect_xml))
		goto fail;
	reply = recv_plist(s);
	if (!reply)
		goto fail;

	long result = -1;
	find_int_after(reply, "Number", &result);
	bfree(reply);
	reply = NULL;
	if (result != 0)
		goto fail; /* device present but app not listening (yet) */

	/* From here the socket is a raw pipe to the app. */
	set_timeouts(s, 0);
	net_set_nonblocking(s);
	return s;

fail:
	net_close(s);
	return OBSC_INVALID_SOCKET;
}

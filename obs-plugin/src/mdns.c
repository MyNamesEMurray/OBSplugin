/*
 * Minimal one-shot mDNS-SD browser (RFC 6762 §5.1, "one-shot multicast
 * DNS queries"). Sends a PTR question for a service type to 224.0.0.251
 * from an *ephemeral* UDP port with the unicast-response (QU) bit set;
 * per the RFC, responders reply via unicast straight to our port. That
 * sidesteps binding port 5353 or joining the multicast group — both of
 * which fight the OS's own Bonjour/Avahi daemon — and makes the same
 * plain-socket code work on Windows, macOS, and Linux.
 *
 * Deliberately small: we only need each phone's NAME and ADDRESS. The
 * wire port is fixed by the protocol, and the reply's source address IS
 * the device, so the parser just walks answer records for PTR matches of
 * the queried type and pulls the instance label; SRV/TXT/A records are
 * ignored. Multicast queries go out the default-route interface only —
 * fine for the single-LAN setups LensLink targets.
 */

#include <stdbool.h> /* net-compat.h uses bool; include first */
#include <string.h>
#include <stdio.h>

#include "net-compat.h"
#include "mdns.h"

#include <util/platform.h>
#include <util/dstr.h> /* astrcmpi */

/* Overridable so a test harness can point the query at loopback
 * (containers/CI often have no multicast route). */
#ifndef MDNS_ADDR
#define MDNS_ADDR "224.0.0.251"
#endif
#define MDNS_PORT 5353
#define DNS_TYPE_PTR 12
#define DNS_CLASS_IN 1
#define DNS_UNICAST_RESPONSE 0x8000
#define DNS_MAX_JUMPS 16

/* Encodes a dotted name into DNS labels; returns bytes written or -1. */
static int dns_encode_name(const char *name, uint8_t *out, size_t out_size)
{
	size_t o = 0;

	while (*name) {
		const char *dot = strchr(name, '.');
		size_t label = dot ? (size_t)(dot - name) : strlen(name);
		if (label == 0 || label > 63 || o + label + 1 >= out_size)
			return -1;
		out[o++] = (uint8_t)label;
		memcpy(out + o, name, label);
		o += label;
		name += label;
		if (*name == '.')
			name++;
	}
	if (o + 1 > out_size)
		return -1;
	out[o++] = 0;
	return (int)o;
}

/* Reads a (possibly compressed) name at `off`, writing it dot-joined into
 * `out` (optional, truncating). Returns the offset just past the name at
 * its original site, or 0 on malformed input. Jump-bounded so crafted
 * pointer loops can't spin us. */
static size_t dns_read_name(const uint8_t *msg, size_t len, size_t off,
			    char *out, size_t out_size)
{
	size_t next = 0;
	size_t o = 0;
	int jumps = 0;

	while (off < len) {
		uint8_t c = msg[off];
		if (c == 0) {
			if (!next)
				next = off + 1;
			break;
		}
		if ((c & 0xC0) == 0xC0) {
			if (off + 1 >= len || ++jumps > DNS_MAX_JUMPS)
				return 0;
			if (!next)
				next = off + 2;
			off = ((size_t)(c & 0x3F) << 8) | msg[off + 1];
			continue;
		}
		if (c > 63 || off + 1 + c > len)
			return 0;
		if (out && out_size) {
			if (o && o + 1 < out_size)
				out[o++] = '.';
			for (uint8_t i = 0; i < c && o + 1 < out_size; i++)
				out[o++] = (char)msg[off + 1 + i];
		}
		off += 1 + c;
	}
	if (out && out_size)
		out[o < out_size ? o : out_size - 1] = 0;
	return next;
}

/* Copies the first label of the name at `off` (following any compression
 * pointers) — the service *instance*, i.e. the phone's name. A label is
 * raw bytes, so names containing dots survive where splitting the joined
 * string would not. */
static bool dns_first_label(const uint8_t *msg, size_t len, size_t off,
			    char *out, size_t out_size)
{
	int jumps = 0;

	while (off < len && (msg[off] & 0xC0) == 0xC0) {
		if (off + 1 >= len || ++jumps > DNS_MAX_JUMPS)
			return false;
		off = ((size_t)(msg[off] & 0x3F) << 8) | msg[off + 1];
	}
	if (off >= len)
		return false;
	uint8_t c = msg[off];
	if (c == 0 || c > 63 || off + 1 + c > len || out_size == 0)
		return false;
	size_t n = c < out_size - 1 ? c : out_size - 1;
	memcpy(out, msg + off + 1, n);
	out[n] = 0;
	return true;
}

/* Scans one response packet for PTR answers matching `service_type`,
 * appending unique hosts to `results`. Returns the updated count. */
static int parse_response(const uint8_t *msg, size_t len,
			  const char *service_type,
			  const struct sockaddr_in *from,
			  struct mdns_result *results, int count,
			  int max_results)
{
	if (len < 12)
		return count;

	char host[64];
	if (!inet_ntop(AF_INET, &from->sin_addr, host, sizeof(host)))
		return count;
	for (int i = 0; i < count; i++) {
		if (strcmp(results[i].host, host) == 0)
			return count; /* already have this phone */
	}

	unsigned qdcount = ((unsigned)msg[4] << 8) | msg[5];
	unsigned ancount = ((unsigned)msg[6] << 8) | msg[7];

	size_t off = 12;
	for (unsigned i = 0; i < qdcount; i++) {
		off = dns_read_name(msg, len, off, NULL, 0);
		if (!off || off + 4 > len)
			return count;
		off += 4;
	}

	for (unsigned i = 0; i < ancount && count < max_results; i++) {
		char rname[256];
		off = dns_read_name(msg, len, off, rname, sizeof(rname));
		if (!off || off + 10 > len)
			return count;
		unsigned type = ((unsigned)msg[off] << 8) | msg[off + 1];
		unsigned rdlen = ((unsigned)msg[off + 8] << 8) | msg[off + 9];
		size_t rdata = off + 10;
		off = rdata + rdlen;
		if (off > len)
			return count;

		if (type != DNS_TYPE_PTR || astrcmpi(rname, service_type) != 0)
			continue;

		struct mdns_result *r = &results[count];
		if (!dns_first_label(msg, len, rdata, r->name,
				     sizeof(r->name)))
			continue;
		snprintf(r->host, sizeof(r->host), "%s", host);
		count++;
	}
	return count;
}

int mdns_browse(const char *service_type, int timeout_ms,
		struct mdns_result *results, int max_results)
{
	uint8_t query[160];
	uint8_t qname[128];

	int name_len = dns_encode_name(service_type, qname, sizeof(qname));
	if (name_len < 0 || max_results <= 0 || timeout_ms <= 0)
		return 0;

	memset(query, 0, 12);
	query[5] = 1; /* qdcount */
	memcpy(query + 12, qname, (size_t)name_len);
	size_t qlen = 12 + (size_t)name_len;
	query[qlen++] = 0;
	query[qlen++] = DNS_TYPE_PTR;
	query[qlen++] = (DNS_CLASS_IN | DNS_UNICAST_RESPONSE) >> 8;
	query[qlen++] = DNS_CLASS_IN & 0xFF;

	socket_t sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock == OBSC_INVALID_SOCKET)
		return 0;
	net_set_nonblocking(sock);

	struct sockaddr_in dest = {0};
	dest.sin_family = AF_INET;
	dest.sin_port = htons(MDNS_PORT);
	inet_pton(AF_INET, MDNS_ADDR, &dest.sin_addr);

	int count = 0;
	uint64_t deadline =
		os_gettime_ns() + (uint64_t)timeout_ms * 1000000ULL;
	uint64_t next_send = 0;

	while (count < max_results) {
		uint64_t now = os_gettime_ns();
		if (now >= deadline)
			break;

		if (now >= next_send) {
			sendto(sock, (const char *)query, (int)qlen, 0,
			       (struct sockaddr *)&dest, sizeof(dest));
			/* One retransmit halfway through the window covers
			 * a lost first query. */
			next_send = now + (uint64_t)timeout_ms * 500000ULL;
		}

		int remaining_ms = (int)((deadline - now) / 1000000ULL);
		int wait_ms = remaining_ms < 100 ? remaining_ms : 100;
		if (net_wait(sock, NET_WAIT_READ, wait_ms > 0 ? wait_ms : 1) <=
		    0)
			continue;

		uint8_t buf[1500];
		struct sockaddr_in from;
		socklen_t from_len = sizeof(from);
		int n = (int)recvfrom(sock, (char *)buf, sizeof(buf), 0,
				      (struct sockaddr *)&from, &from_len);
		if (n <= 0)
			continue;
		count = parse_response(buf, (size_t)n, service_type, &from,
				       results, count, max_results);
	}

	net_close(sock);
	return count;
}

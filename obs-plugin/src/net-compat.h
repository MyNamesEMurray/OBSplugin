/*
 * Minimal cross-platform (POSIX / Winsock) socket compatibility layer.
 */

#pragma once

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>

typedef SOCKET socket_t;
#define OBSC_INVALID_SOCKET INVALID_SOCKET

static inline void net_close(socket_t s)
{
	closesocket(s);
}

static inline bool net_init(void)
{
	WSADATA wsa;
	return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

static inline void net_shutdown(void)
{
	WSACleanup();
}

#else /* POSIX */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

typedef int socket_t;
#define OBSC_INVALID_SOCKET (-1)

static inline void net_close(socket_t s)
{
	close(s);
}

static inline bool net_init(void)
{
	return true;
}

static inline void net_shutdown(void)
{
}

#endif

static inline int net_last_error(void)
{
#ifdef _WIN32
	return WSAGetLastError();
#else
	return errno;
#endif
}

#ifndef _WIN32
#include <poll.h>
#endif

#define NET_WAIT_READ 1
#define NET_WAIT_WRITE 2

/*
 * Waits until the socket is readable/writable or the timeout expires.
 * Returns a bitmask of NET_WAIT_* (0 on timeout, -1 on error). POSIX uses
 * poll(): select()'s fd_set is undefined behavior for fds >= FD_SETSIZE
 * (1024), reachable in a big OBS setup. Windows select() has no such
 * limit (fd_sets are arrays of handles), so it stays.
 */
static inline int net_wait(socket_t s, int events, int timeout_ms)
{
#ifdef _WIN32
	fd_set rfds, wfds;
	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	if (events & NET_WAIT_READ)
		FD_SET(s, &rfds);
	if (events & NET_WAIT_WRITE)
		FD_SET(s, &wfds);
	struct timeval tv = {.tv_sec = timeout_ms / 1000,
			     .tv_usec = (timeout_ms % 1000) * 1000};
	int r = select(0, (events & NET_WAIT_READ) ? &rfds : NULL,
		       (events & NET_WAIT_WRITE) ? &wfds : NULL, NULL, &tv);
	if (r <= 0)
		return r;
	int out = 0;
	if (FD_ISSET(s, &rfds))
		out |= NET_WAIT_READ;
	if (FD_ISSET(s, &wfds))
		out |= NET_WAIT_WRITE;
	return out;
#else
	struct pollfd pfd = {.fd = s, .events = 0};
	if (events & NET_WAIT_READ)
		pfd.events |= POLLIN;
	if (events & NET_WAIT_WRITE)
		pfd.events |= POLLOUT;
	int r = poll(&pfd, 1, timeout_ms);
	if (r <= 0)
		return r;
	int out = 0;
	if (pfd.revents & (POLLIN | POLLERR | POLLHUP))
		out |= NET_WAIT_READ;
	if (pfd.revents & (POLLOUT | POLLERR | POLLHUP))
		out |= NET_WAIT_WRITE;
	return out;
#endif
}

/* True if the last send/recv failed only because it would block. */
static inline bool net_would_block(void)
{
#ifdef _WIN32
	return WSAGetLastError() == WSAEWOULDBLOCK;
#else
	return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

static inline bool net_set_nonblocking(socket_t s)
{
#ifdef _WIN32
	u_long mode = 1;
	return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
	int flags = fcntl(s, F_GETFL, 0);
	if (flags < 0)
		return false;
	return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

/* Windows sockets accepted from a non-blocking listener inherit
 * non-blocking mode; use this to restore blocking I/O on them. */
static inline bool net_set_blocking(socket_t s)
{
#ifdef _WIN32
	u_long mode = 0;
	return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
	int flags = fcntl(s, F_GETFL, 0);
	if (flags < 0)
		return false;
	return fcntl(s, F_SETFL, flags & ~O_NONBLOCK) == 0;
#endif
}

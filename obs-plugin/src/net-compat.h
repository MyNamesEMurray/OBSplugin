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

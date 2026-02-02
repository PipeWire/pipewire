#ifndef NETWORK_UTILS_H
#define NETWORK_UTILS_H

#include <stdio.h>
#include <netdb.h>
#include <string.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/un.h>

#include <spa/utils/string.h>

#ifdef __FreeBSD__
#define ifr_ifindex ifr_index
#endif

static inline int pw_net_parse_address(const char *address, uint16_t port,
		struct sockaddr_storage *addr, socklen_t *len)
{
	struct addrinfo hints;
	struct addrinfo *result, *rp;
	int res;
	char port_str[6];

	snprintf(port_str, sizeof(port_str), "%u", port);

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_NUMERICSERV;

	res = getaddrinfo(address, port_str, &hints, &result);

	if (res != 0)
		return -EINVAL;

	for (rp = result; rp != NULL; rp = rp->ai_next) {
		memcpy(addr, rp->ai_addr, rp->ai_addrlen);
		*len = rp->ai_addrlen;
		break;
	}
	freeaddrinfo(result);

	return 0;
}

static inline uint16_t pw_net_parse_port(const char *str, uint16_t def)
{
	uint32_t val;
	if (spa_atou32(str, &val, 0) && val <= 65535u)
		return val;
	return def;
}

static inline int pw_net_parse_address_port(const char *address,
		const char *default_address, uint16_t default_port,
		struct sockaddr_storage *addr, socklen_t *len)
{
	uint16_t port;
	char *br = NULL, *col, *n;

	n = strdupa(address);

	col = strrchr(n, ':');
	if (n[0] == '[') {
		br = strchr(n, ']');
		if (br == NULL)
			return -EINVAL;
		n++;
		*br = 0;
	}
	if (br && col && col < br)
		col = NULL;

	if (col) {
		*col = '\0';
		port = pw_net_parse_port(col+1, default_port);
	} else {
		port = pw_net_parse_port(n, default_port);
		n = strdupa(default_address ? default_address : "0.0.0.0");
	}
	return pw_net_parse_address(n, port, addr, len);
}

static inline int pw_net_get_ip(const struct sockaddr_storage *sa, char *ip, size_t len, bool *ip4, uint16_t *port)
{
	if (ip4)
		*ip4 = sa->ss_family == AF_INET;

	if (sa->ss_family == AF_INET) {
		struct sockaddr_in *in = (struct sockaddr_in*)sa;
		if (inet_ntop(sa->ss_family, &in->sin_addr, ip, len) == NULL)
			return -errno;
		if (port)
			*port = ntohs(in->sin_port);
	} else if (sa->ss_family == AF_INET6) {
		struct sockaddr_in6 *in = (struct sockaddr_in6*)sa;
		if (inet_ntop(sa->ss_family, &in->sin6_addr, ip, len) == NULL)
			return -errno;
		if (port)
			*port = ntohs(in->sin6_port);

		if (in->sin6_scope_id != 0 && len > 1) {
			size_t curlen = strlen(ip);
			if (len-(curlen+1) >= IFNAMSIZ) {
				ip += curlen+1;
				ip[-1] = '%';
				if (if_indextoname(in->sin6_scope_id, ip) == NULL)
					ip[-1] = 0;
			}
		}
	} else
		return -EINVAL;
	return 0;
}

static inline char *pw_net_get_ip_fmt(const struct sockaddr_storage *sa, char *ip, size_t len)
{
	if (pw_net_get_ip(sa, ip, len, NULL, NULL) != 0)
		snprintf(ip, len, "invalid ip");
	return ip;
}

static inline bool pw_net_addr_is_any(struct sockaddr_storage *addr)
{
	if (addr->ss_family == AF_INET) {
		struct sockaddr_in *sa = (struct sockaddr_in*)addr;
		return sa->sin_addr.s_addr == INADDR_ANY;
	} else if (addr->ss_family == AF_INET6) {
		struct sockaddr_in6 *sa = (struct sockaddr_in6*)addr;
		return memcmp(&sa->sin6_addr, &in6addr_any, sizeof(sa->sin6_addr));
	}
	return false;
}

#ifndef LISTEN_FDS_START
#define LISTEN_FDS_START 3
#endif

/* Returns the number of file descriptors passed for socket activation.
 * Returns 0 if none, -1 on error. */
static inline int listen_fd(void)
{
	uint32_t n;
	int i, flags;

	if (!spa_atou32(getenv("LISTEN_FDS"), &n, 10) || n > INT_MAX - LISTEN_FDS_START) {
		errno = EINVAL;
		return -1;
	}

	for (i = 0; i < (int)n; i++) {
		flags = fcntl(LISTEN_FDS_START + i, F_GETFD);
		if (flags == -1)
			return -1;
		if (fcntl(LISTEN_FDS_START + i, F_SETFD, flags | FD_CLOEXEC) == -1)
			return -1;
	}

	unsetenv("LISTEN_FDS");

	return (int)n;
}

/* Check if the fd is a listening unix socket of the given type,
 * optionally bound to the given path. */
static inline int is_socket_unix(int fd, int type, const char *path)
{
	struct sockaddr_un addr;
	int val;
	socklen_t len = sizeof(val);

	if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &val, &len) < 0)
		return -errno;
	if (val != type)
		return 0;

	if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &val, &len) < 0)
		return -errno;
	if (!val)
		return 0;

	if (path) {
		len = sizeof(addr);
		memset(&addr, 0, sizeof(addr));
		if (getsockname(fd, (struct sockaddr *)&addr, &len) < 0)
			return -errno;
		if (addr.sun_family != AF_UNIX)
			return 0;
		size_t length = strlen(path);
		if (length > 0) {
			if (len < offsetof(struct sockaddr_un, sun_path) + length)
				return 0;
			if (memcmp(addr.sun_path, path, length) != 0)
				return 0;
		}
	}

	return 1;
}

#endif /* NETWORK_UTILS_H */

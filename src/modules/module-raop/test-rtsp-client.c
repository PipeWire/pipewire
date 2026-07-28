/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Lukáš Lipinský */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <pipewire/pipewire.h>

#include "rtsp-client.h"

struct test_data {
	struct pw_rtsp_client *client;
	bool connected;
	int error;
	unsigned int replies;
	unsigned int disconnects;
};

static void client_connected(void *data)
{
	struct test_data *test = data;

	test->connected = true;
}

static void client_error(void *data, int res)
{
	struct test_data *test = data;

	test->error = res;
}

static void client_disconnected(void *data)
{
	struct test_data *test = data;

	test->disconnects++;
}

static int reply_disconnect(void *data, int status,
		const struct spa_dict *headers, const struct pw_array *content)
{
	struct test_data *test = data;

	spa_assert_se(status == 200);
	spa_assert_se(spa_streq(spa_dict_lookup(headers, "Connection"), "close"));
	test->replies++;
	spa_assert_se(pw_rtsp_client_disconnect(test->client) == 0);
	return 0;
}

static const struct pw_rtsp_client_events client_events = {
	PW_VERSION_RTSP_CLIENT_EVENTS,
	.connected = client_connected,
	.error = client_error,
	.disconnected = client_disconnected,
};

static int create_server(uint16_t *port)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	socklen_t len = sizeof(addr);
	int fd;

	fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	spa_assert_se(fd >= 0);
	spa_assert_se(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	spa_assert_se(listen(fd, 1) == 0);
	spa_assert_se(getsockname(fd, (struct sockaddr *)&addr, &len) == 0);
	*port = ntohs(addr.sin_port);
	return fd;
}

static void iterate_until(struct pw_loop *loop, const bool *condition)
{
	unsigned int i;

	for (i = 0; i < 100 && !*condition; i++)
		spa_assert_se(pw_loop_iterate(loop, 10) >= 0);
	spa_assert_se(*condition);
}

static void send_all(struct pw_loop *loop, int fd, const char *data, size_t size)
{
	size_t offset = 0;
	unsigned int i;

	for (i = 0; i < 100 && offset < size; i++) {
		ssize_t res = send(fd, data + offset, size - offset, MSG_NOSIGNAL);

		if (res < 0) {
			if (errno == EINTR)
				continue;
			spa_assert_se(errno == EAGAIN || errno == EWOULDBLOCK);
			spa_assert_se(pw_loop_iterate(loop, 10) >= 0);
			continue;
		}
		spa_assert_se(res > 0);
		offset += res;
	}
	spa_assert_se(offset == size);
}

static void test_reply_callback_disconnect(void)
{
	char request[2048] = { 0 }, response[128];
	struct pw_main_loop *main_loop;
	struct pw_loop *loop;
	struct pw_rtsp_client *client;
	struct spa_hook listener;
	struct test_data test = { 0 };
	uint16_t port;
	size_t offset = 0;
	int server_fd, peer_fd = -1;
	unsigned int i;

	server_fd = create_server(&port);
	main_loop = pw_main_loop_new(NULL);
	spa_assert_se(main_loop != NULL);
	loop = pw_main_loop_get_loop(main_loop);
	client = pw_rtsp_client_new(loop, pw_properties_new(NULL, NULL), 0);
	spa_assert_se(client != NULL);
	test.client = client;
	pw_rtsp_client_add_listener(client, &listener, &client_events, &test);

	pw_loop_enter(loop);
	spa_assert_se(pw_rtsp_client_connect(client,
			"127.0.0.1", port, "test") == 0);
	iterate_until(loop, &test.connected);
	spa_assert_se(test.error == 0);

	for (i = 0; i < 100 && peer_fd < 0; i++) {
		peer_fd = accept4(server_fd, NULL, NULL,
				SOCK_CLOEXEC | SOCK_NONBLOCK);
		if (peer_fd < 0) {
			if (errno == EINTR)
				continue;
			spa_assert_se(errno == EAGAIN || errno == EWOULDBLOCK);
			spa_assert_se(pw_loop_iterate(loop, 10) >= 0);
		}
	}
	spa_assert_se(peer_fd >= 0);

	spa_assert_se(pw_rtsp_client_send(client, "TEARDOWN", NULL,
			NULL, NULL, reply_disconnect, &test) == 0);
	for (i = 0; i < 100 && strstr(request, "\r\n\r\n") == NULL; i++) {
		ssize_t res;

		spa_assert_se(pw_loop_iterate(loop, 10) >= 0);
		res = recv(peer_fd, request + offset,
				sizeof(request) - offset - 1, 0);
		if (res < 0) {
			if (errno == EINTR)
				continue;
			spa_assert_se(errno == EAGAIN || errno == EWOULDBLOCK);
			continue;
		}
		spa_assert_se(res > 0);
		offset += res;
		spa_assert_se(offset < sizeof(request));
		request[offset] = '\0';
	}
	spa_assert_se(strstr(request, "\r\n\r\n") != NULL);
	spa_assert_se(strstr(request, "\r\nCSeq: 1\r\n") != NULL);

	spa_scnprintf(response, sizeof(response),
			"RTSP/1.0 200 OK\r\n"
			"CSeq: 1\r\n"
			"Connection: close\r\n"
			"Content-Length: 0\r\n\r\n");
	send_all(loop, peer_fd, response, strlen(response));

	for (i = 0; i < 100 && test.replies == 0; i++)
		spa_assert_se(pw_loop_iterate(loop, 10) >= 0);
	spa_assert_se(test.replies == 1);
	spa_assert_se(test.disconnects == 1);
	spa_assert_se(test.error == 0);

	pw_rtsp_client_destroy(client);
	pw_loop_leave(loop);
	pw_main_loop_destroy(main_loop);
	close(peer_fd);
	close(server_fd);
}

int main(int argc, char *argv[])
{
	pw_init(&argc, &argv);
	test_reply_callback_disconnect();
	pw_deinit();
	return 0;
}

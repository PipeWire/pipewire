/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Fabian Schmieder */
/* SPDX-License-Identifier: MIT */

/* Exercise the auth-setup challenge retry and its Digest request target. */
#include "config.h"
#include <pipewire/impl.h>

/* Observe rejection without creating a module; RTSP I/O remains real. */
static unsigned int destroy_count;

static void test_schedule_destroy(struct pw_impl_module *module)
{
	destroy_count++;
}

#define pw_impl_module_schedule_destroy test_schedule_destroy
#include "../module-raop-sink.c"
#undef pw_impl_module_schedule_destroy

#define TEST_PASSWORD "test-password"
#define TEST_REALM "airplay"
#define TEST_NONCE "test-nonce"

static const uint8_t auth_setup_content[33] = {
	0x01,
	0x59, 0x02, 0xed, 0xe9, 0x0d, 0x4e, 0xf2, 0xbd,
	0x4c, 0xb6, 0x8a, 0x63, 0x30, 0x03, 0x82, 0x07,
	0xa9, 0x4d, 0xbd, 0x50, 0xd8, 0xaa, 0x46, 0x5b,
	0x5d, 0x8c, 0x01, 0x2a, 0x0c, 0x7e, 0x1d, 0x4e };

struct test_data {
	bool connected;
	int error;
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

static const struct pw_rtsp_client_events client_events = {
	PW_VERSION_RTSP_CLIENT_EVENTS,
	.connected = client_connected,
	.error = client_error,
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

static ssize_t header_end(const uint8_t *data, size_t size)
{
	size_t i;

	for (i = 0; i + 4 <= size; i++) {
		if (memcmp(data + i, "\r\n\r\n", 4) == 0)
			return (ssize_t)i;
	}
	return -1;
}

static size_t request_size(const uint8_t *data, size_t size)
{
	ssize_t end = header_end(data, size);
	const char *length;
	size_t body = 0;

	if (end < 0)
		return 0;
	length = strstr((const char *)data, "\r\nContent-Length: ");
	if (length != NULL && length < (const char *)data + end)
		spa_assert_se(sscanf(length, "\r\nContent-Length: %zu", &body) == 1);
	return (size_t)end + 4 + body;
}

static void read_request(struct pw_loop *loop, int fd, uint8_t *request, size_t capacity,
		size_t *size)
{
	unsigned int i;

	*size = 0;
	for (i = 0; i < 100; i++) {
		ssize_t res;
		size_t needed = request_size(request, *size);

		if (needed != 0 && needed <= *size)
			break;

		spa_assert_se(pw_loop_iterate(loop, 10) >= 0);
		spa_assert_se(destroy_count == 0);
		spa_assert_se(*size < capacity - 1);
		res = recv(fd, request + *size, capacity - *size - 1, 0);
		if (res < 0) {
			spa_assert_se(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
			continue;
		}
		spa_assert_se(res > 0);
		*size += (size_t)res;
		request[*size] = '\0';
	}
	spa_assert_se(request_size(request, *size) == *size);
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
		offset += (size_t)res;
	}
	spa_assert_se(offset == size);
}

static void md5_text(char hash[MD5_HASH_LENGTH + 1], const char *text)
{
	unsigned char digest[MD5_DIGEST_LENGTH];
	unsigned int size = sizeof(digest);
	unsigned int i;

	spa_assert_se(EVP_Digest(text, strlen(text), digest, &size, EVP_md5(), NULL) == 1);
	spa_assert_se(size == sizeof(digest));
	for (i = 0; i < sizeof(digest); i++)
		spa_scnprintf(&hash[2 * i], 3, "%02x", digest[i]);
	hash[MD5_HASH_LENGTH] = '\0';
}

static void expected_digest(char *auth, size_t size, const char *method, const char *url)
{
	char input[256];
	char h1[MD5_HASH_LENGTH + 1];
	char h2[MD5_HASH_LENGTH + 1];
	char response[MD5_HASH_LENGTH + 1];

	spa_scnprintf(input, sizeof(input), "%s:%s:%s", RAOP_AUTH_USER_NAME,
			TEST_REALM, TEST_PASSWORD);
	md5_text(h1, input);
	spa_scnprintf(input, sizeof(input), "%s:%s", method, url);
	md5_text(h2, input);
	spa_scnprintf(input, sizeof(input), "%s:%s:%s", h1, TEST_NONCE, h2);
	md5_text(response, input);
	spa_scnprintf(auth, size,
			"Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\"",
			RAOP_AUTH_USER_NAME, TEST_REALM, TEST_NONCE, url, response);
}

static void expected_basic(char *auth, size_t size)
{
	const char credentials[] = RAOP_AUTH_USER_NAME ":" TEST_PASSWORD;
	unsigned char encoded[256];
	int length = EVP_EncodeBlock(encoded, (const unsigned char *)credentials,
			sizeof(credentials) - 1);

	spa_assert_se(length > 0 && (size_t)length < sizeof(encoded));
	encoded[length] = '\0';
	spa_scnprintf(auth, size, "Basic %s", encoded);
}

static void assert_headers(const uint8_t *request, const char *method, const char *url,
		const char *authorization, unsigned int cseq)
{
	char cseq_header[32];
	char request_line[1024];

	spa_scnprintf(cseq_header, sizeof(cseq_header), "\r\nCSeq: %u\r\n", cseq);
	spa_scnprintf(request_line, sizeof(request_line), "%s %s RTSP/1.0\r\n", method, url);
	spa_assert_se(strncmp((const char *)request, request_line, strlen(request_line)) == 0);
	spa_assert_se(strstr((const char *)request, cseq_header) != NULL);
	if (authorization != NULL) {
		char header[1024];
		spa_scnprintf(header, sizeof(header), "\r\nAuthorization: %s\r\n", authorization);
		spa_assert_se(strstr((const char *)request, header) != NULL);
	}
	else
		spa_assert_se(strstr((const char *)request, "\r\nAuthorization:") == NULL);
}

static void assert_request(const uint8_t *request, size_t size, const char *authorization,
		unsigned int cseq)
{
	ssize_t end;

	assert_headers(request, "POST", "/auth-setup", authorization, cseq);
	spa_assert_se(strstr((const char *)request, "\r\nContent-Type: application/octet-stream\r\n") != NULL);
	spa_assert_se(strstr((const char *)request, "\r\nContent-Length: 33\r\n") != NULL);

	end = header_end(request, size);
	spa_assert_se(end >= 0);
	spa_assert_se(size == (size_t)end + 4 + sizeof(auth_setup_content));
	spa_assert_se(memcmp(request + end + 4, auth_setup_content, sizeof(auth_setup_content)) == 0);
}

static void reply(struct pw_loop *loop, int fd, unsigned int cseq, int status,
		const char *challenge)
{
	char response[1024];
	int length = spa_scnprintf(response, sizeof(response),
			"RTSP/1.0 %d %s\r\nCSeq: %u\r\n%s%s%sContent-Length: 0\r\n\r\n",
			status, status == 200 ? "OK" : "Unauthorized", cseq,
			challenge ? "WWW-Authenticate: " : "", challenge ? challenge : "",
			challenge ? "\r\n" : "");

	send_all(loop, fd, response, length);
}

static void test_auth_setup(bool accepted, const char *options_auth)
{
	struct test_data test = { 0 };
	struct pw_main_loop *main_loop;
	struct pw_loop *loop;
	struct pw_rtsp_client *client;
	struct spa_hook listener;
	struct impl sender = { 0 };
	uint8_t request[4096] = { 0 };
	char authorization[1024];
	uint16_t port;
	size_t size;
	int server_fd, peer_fd = -1;
	unsigned int post_cseq;
	const bool basic = options_auth && spa_streq(options_auth, "Basic");
	const char *challenge = basic ? "Basic realm=\"" TEST_REALM "\"" :
			"Digest realm=\"" TEST_REALM "\", nonce=\"" TEST_NONCE "\"";

	destroy_count = 0;
	server_fd = create_server(&port);
	main_loop = pw_main_loop_new(NULL);
	spa_assert_se(main_loop != NULL);
	loop = pw_main_loop_get_loop(main_loop);
	client = pw_rtsp_client_new(loop, pw_properties_new(NULL, NULL), 0);
	spa_assert_se(client != NULL);
	pw_rtsp_client_add_listener(client, &listener, &client_events, &test);

	sender.props = pw_properties_new("raop.ip", "127.0.0.1", NULL);
	sender.headers = pw_properties_new(NULL, NULL);
	sender.password = strdup(TEST_PASSWORD);
	sender.encryption = options_auth ? CRYPTO_AUTH_SETUP : CRYPTO_NONE;
	sender.psamples = 352;
	sender.rate = 44100;
	sender.rtsp = client;

	pw_loop_enter(loop);
	spa_assert_se(pw_rtsp_client_connect(client, "127.0.0.1", port, "test") == 0);
	iterate_until(loop, &test.connected);
	spa_assert_se(test.error == 0);

	for (unsigned int i = 0; i < 100 && peer_fd < 0; i++) {
		peer_fd = accept4(server_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
		if (peer_fd < 0) {
			spa_assert_se(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
			spa_assert_se(pw_loop_iterate(loop, 10) >= 0);
		}
	}
	spa_assert_se(peer_fd >= 0);

	if (options_auth) {
		const char *url = pw_rtsp_client_get_url(client);

		spa_assert_se(accepted);
		rtsp_connected(&sender);
		read_request(loop, peer_fd, request, sizeof(request), &size);
		assert_headers(request, "OPTIONS", url, NULL, 1);
		reply(loop, peer_fd, 1, 401, challenge);
		if (basic)
			expected_basic(authorization, sizeof(authorization));
		else
			expected_digest(authorization, sizeof(authorization), "OPTIONS", url);
		memset(request, 0, sizeof(request));
		read_request(loop, peer_fd, request, sizeof(request), &size);
		assert_headers(request, "OPTIONS", url, authorization, 2);
		reply(loop, peer_fd, 2, 200, NULL);
		post_cseq = 3;
	} else {
		spa_assert_se(rtsp_do_post_auth_setup(&sender) == 0);
		read_request(loop, peer_fd, request, sizeof(request), &size);
		assert_request(request, size, NULL, 1);
		reply(loop, peer_fd, 1, 401, challenge);
		post_cseq = 2;
	}
	if (basic)
		expected_basic(authorization, sizeof(authorization));
	else
		expected_digest(authorization, sizeof(authorization), "POST", "/auth-setup");
	memset(request, 0, sizeof(request));
	read_request(loop, peer_fd, request, sizeof(request), &size);
	assert_request(request, size, authorization, post_cseq);
	if (accepted) {
		reply(loop, peer_fd, post_cseq, 200, NULL);
		spa_assert_se(pw_loop_iterate(loop, 10) >= 0);
		spa_assert_se(destroy_count == 0);
		memset(request, 0, sizeof(request));
		read_request(loop, peer_fd, request, sizeof(request), &size);
		if (basic)
			expected_basic(authorization, sizeof(authorization));
		else
			expected_digest(authorization, sizeof(authorization), "ANNOUNCE",
					pw_rtsp_client_get_url(client));
		assert_headers(request, "ANNOUNCE", pw_rtsp_client_get_url(client),
				authorization, post_cseq + 1);
	} else {
		reply(loop, peer_fd, post_cseq, 401, challenge);
		for (unsigned int i = 0; i < 100 && destroy_count == 0; i++)
			spa_assert_se(pw_loop_iterate(loop, 10) >= 0);
		spa_assert_se(destroy_count == 1);
		/* Neither a third POST nor ANNOUNCE may be queued after rejection. */
		spa_assert_se(pw_loop_iterate(loop, 10) >= 0);
		spa_assert_se(recv(peer_fd, request, sizeof(request), MSG_DONTWAIT) < 0);
		spa_assert_se(errno == EAGAIN || errno == EWOULDBLOCK);
	}
	spa_assert_se(test.error == 0);

	free(sender.auth_method);
	if (sender.realm) {
		explicit_bzero(sender.realm, strlen(sender.realm));
		free(sender.realm);
	}
	if (sender.nonce) {
		explicit_bzero(sender.nonce, strlen(sender.nonce));
		free(sender.nonce);
	}
	free(sender.password);
	pw_properties_free(sender.props);
	pw_properties_free(sender.headers);
	pw_rtsp_client_destroy(client);
	pw_loop_leave(loop);
	pw_main_loop_destroy(main_loop);
	spa_assert_se(close(peer_fd) == 0);
	spa_assert_se(close(server_fd) == 0);
}

static void test_invalid_challenge(const char *challenge, bool password, bool options)
{
	struct impl sender = { .password = password ? TEST_PASSWORD : NULL, .connected = true };
	struct pw_main_loop *main_loop = pw_main_loop_new(NULL);
	const struct spa_dict_item items[] = { { "WWW-Authenticate", challenge } };
	const struct spa_dict headers = SPA_DICT_INIT_ARRAY(items);

	spa_assert_se(main_loop != NULL);
	sender.rtsp = pw_rtsp_client_new(pw_main_loop_get_loop(main_loop),
			pw_properties_new(NULL, NULL), 0);
	spa_assert_se(sender.rtsp != NULL);
	sender.headers = pw_properties_new(NULL, NULL);
	destroy_count = 0;
	if (options)
		spa_assert_se(rtsp_options_reply(&sender, 401, &headers, NULL) < 0);
	else
		spa_assert_se(rtsp_post_auth_setup_reply(&sender, 401, &headers, NULL) <= 0);
	spa_assert_se(destroy_count == 1);
	free(sender.auth_method);
	free(sender.realm);
	free(sender.nonce);
	pw_properties_free(sender.headers);
	pw_rtsp_client_destroy(sender.rtsp);
	pw_main_loop_destroy(main_loop);
}

int main(int argc, char **argv)
{
	pw_init(&argc, &argv);
	test_auth_setup(true, NULL);
	test_auth_setup(false, NULL);
	test_auth_setup(true, "Digest");
	test_auth_setup(true, "Basic");
	for (unsigned int options = 0; options < 2; options++) {
		test_invalid_challenge(NULL, true, options);
		test_invalid_challenge("Digest realm=\"airplay\"", true, options);
		test_invalid_challenge("Bearer test-token", true, options);
		test_invalid_challenge("Digest realm=\"airplay\", nonce=\"test-nonce\"", false, options);
	}
	pw_deinit();
	return EXIT_SUCCESS;
}

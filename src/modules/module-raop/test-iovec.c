/* PipeWire */
/* SPDX-FileCopyrightText: Copyright 2026 Fabian Schmieder */
/* SPDX-License-Identifier: MIT */

/* Exercise the actual sender callback and capture its datagrams. */
#include "../module-raop-sink.c"

#define TEST_FRAMES 352u
#define TEST_BYTES (TEST_FRAMES * 4)

static ssize_t packet(struct impl *sender, int receiver, const struct iovec *audio,
		size_t count, uint8_t *output, size_t capacity)
{
	struct rtp_header header = { .v = 2, .pt = 96,
		.sequence_number = htons(23), .timestamp = htonl(44100) };
	struct iovec input[3] = {{ &header, sizeof(header) }};
	ssize_t size;
	size_t i;

	spa_assert_se(count <= 2);
	for (i = 0; i < count; i++)
		input[i + 1] = audio[i];
	stream_send_packet(sender, input, count + 1);
	size = recv(receiver, output, capacity, MSG_DONTWAIT);
	spa_assert_se(size > 0);
	return size;
}

static unsigned int check_splits(struct impl *sender, int receiver)
{
	uint8_t samples[2][TEST_BYTES];
	uint8_t reference[TEST_BYTES + 64], actual[sizeof(reference) + 4];
	uint8_t prefix[4];
	const struct iovec contiguous = { samples[0], TEST_BYTES };
	unsigned int failures = 0;
	ssize_t expected;
	size_t i, split;

	for (i = 0; i < TEST_BYTES; i++)
		samples[0][i] = samples[1][i] = (uint8_t)(i * 73 + i / 256);
	expected = packet(sender, receiver, &contiguous, 1, reference, sizeof(reference));
	spa_assert_se(expected == 12 + TEST_BYTES + 8);
	prefix[0] = '$';
	prefix[1] = 0;
	prefix[2] = (uint8_t)(expected >> 8);
	prefix[3] = (uint8_t)expected;
	for (split = 0; split <= TEST_FRAMES; split++) {
		size_t first = split * 4;
		const struct iovec audio[2] = {
			{ samples[0], first }, { samples[1] + first, TEST_BYTES - first }
		};
		ssize_t size = packet(sender, receiver, audio, 2, actual, sizeof(actual));
		if (size != expected || memcmp(actual, reference, (size_t)expected) != 0)
			failures++;

		/* Capture TCP framing in one datagram; the RTP/ALAC bytes must be unchanged. */
		sender->protocol = PROTO_TCP;
		size = packet(sender, receiver, audio, 2, actual, sizeof(actual));
		sender->protocol = PROTO_UDP;
		if (size != expected + 4 || memcmp(actual, prefix, sizeof(prefix)) != 0 ||
				memcmp(actual + 4, reference, (size_t)expected) != 0)
			failures++;
	}
	return failures;
}

int main(int argc, char **argv)
{
	int sockets[2];
	struct impl sender = { .recording = true, .protocol = PROTO_UDP,
		.codec = CODEC_PCM, .stride = 4, .mtu = 1448, .sync_period = UINT32_MAX };
	unsigned int failures;

	pw_init(&argc, &argv);
	spa_assert_se(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sockets) == 0);
	sender.server_fd = sockets[0];
	failures = check_splits(&sender, sockets[1]);
	spa_assert_se(close(sockets[0]) == 0);
	spa_assert_se(close(sockets[1]) == 0);
	pw_deinit();
	printf("%u cases, %u failures\n", 1 + 2 * (TEST_FRAMES + 1), failures);
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

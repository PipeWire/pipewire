/* PipeWire
 *
 * Copyright © 2019 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <spa/pod/parser.h>

#include <pipewire/pipewire.h>

#include "connection.h"

static void test_create(struct pw_protocol_native_connection *conn)
{
	uint8_t opcode;
	uint32_t dest_id, size;
	void *data;
	int res, seq;

	res = pw_protocol_native_connection_get_next(conn,
			&opcode, &dest_id, &data, &size, &seq);
	spa_assert(res == false);

	res = pw_protocol_native_connection_get_fd(conn, 0);
	spa_assert(res == -1);

	res = pw_protocol_native_connection_flush(conn);
	spa_assert(res == 0);

	res = pw_protocol_native_connection_clear(conn);
	spa_assert(res == 0);
}

static void write_message(struct pw_protocol_native_connection *conn, int fd)
{
	struct spa_pod_builder *b;
	int seq = -1, res;

	b = pw_protocol_native_connection_begin(conn, 1, 5, &seq);
	spa_assert(b != NULL);
	spa_assert(seq != -1);

	spa_pod_builder_add_struct(b,
			SPA_POD_Int(42),
			SPA_POD_Id(SPA_TYPE_Object),
			SPA_POD_Int(pw_protocol_native_connection_add_fd(conn, fd)));

	res = pw_protocol_native_connection_end(conn, b);
	spa_assert(seq == res);
}

static int read_message(struct pw_protocol_native_connection *conn)
{
        struct spa_pod_parser prs;
	uint8_t opcode;
	uint32_t dest_id, size;
	void *data;
	int res, seq, fd;
	uint32_t v_int, v_id, fdidx;

	res = pw_protocol_native_connection_get_next(conn,
			&opcode, &dest_id, &data, &size, &seq);
	if (!res)
		return -1;

	spa_assert(opcode == 5);
	spa_assert(dest_id == 1);
	spa_assert(data != NULL);
	spa_assert(size > 0);

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_get_struct(&prs,
                        SPA_POD_Int(&v_int),
                        SPA_POD_Id(&v_id),
                        SPA_POD_Int(&fdidx)) < 0)
                spa_assert_not_reached();

	fd = pw_protocol_native_connection_get_fd(conn, fdidx);
	pw_log_debug("got fd %d", fd);
	spa_assert(fd != -1);
	return 0;
}

static void test_read_write(struct pw_protocol_native_connection *in,
		struct pw_protocol_native_connection *out)
{
	write_message(out, 1);
	pw_protocol_native_connection_flush(out);
	write_message(out, 2);
	pw_protocol_native_connection_flush(out);
	spa_assert(read_message(in) == 0);
	spa_assert(read_message(in) == 0);
	spa_assert(read_message(in) == -1);

	write_message(out, 1);
	write_message(out, 2);
	pw_protocol_native_connection_flush(out);
	spa_assert(read_message(in) == 0);
	spa_assert(read_message(in) == 0);
	spa_assert(read_message(in) == -1);
}

int main(int argc, char *argv[])
{
	struct pw_main_loop *loop;
	struct pw_core *core;
	struct pw_protocol_native_connection *in, *out;
	int fds[2];

	pw_init(&argc, &argv);

	loop = pw_main_loop_new(NULL);
	core = pw_core_new(pw_main_loop_get_loop(loop), NULL, 0);

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
		spa_assert_not_reached();
		return -1;
	}

	in = pw_protocol_native_connection_new(core, fds[0]);
	spa_assert(in != NULL);
	out = pw_protocol_native_connection_new(core, fds[1]);
	spa_assert(out != NULL);

	test_create(in);
	test_create(out);
	test_read_write(in, out);

	return 0;
}

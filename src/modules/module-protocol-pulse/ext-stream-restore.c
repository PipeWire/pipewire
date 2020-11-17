/* PipeWire
 *
 * Copyright © 2020 Wim Taymans
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

#define EXT_STREAM_RESTORE_VERSION	1

static const struct extension_sub ext_stream_restore[];

static int do_extension_stream_restore_test(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct message *reply;

	reply = reply_new(client, tag);
	message_put(reply,
			TAG_U32, EXT_STREAM_RESTORE_VERSION,
			TAG_INVALID);
	return send_message(client, reply);
}

static int do_extension_stream_restore_read(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct message *reply;
	reply = reply_new(client, tag);
	return send_message(client, reply);
}

static int do_extension_stream_restore_write(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	return reply_simple_ack(client, tag);
}

static int do_extension_stream_restore_delete(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	return reply_simple_ack(client, tag);
}

static int do_extension_stream_restore_subscribe(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	return reply_simple_ack(client, tag);
}

static const struct extension_sub ext_stream_restore[] = {
	{ "TEST", 0, do_extension_stream_restore_test, },
	{ "READ", 1, do_extension_stream_restore_read, },
	{ "WRITE", 2, do_extension_stream_restore_write, },
	{ "DELETE", 3, do_extension_stream_restore_delete, },
	{ "SUBSCRIBE", 4, do_extension_stream_restore_subscribe, },
	{ "EVENT", 5, },
};

static int do_extension_stream_restore(struct client *client, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	uint32_t command;
	int res;

	if ((res = message_get(m,
			TAG_U32, &command,
			TAG_INVALID)) < 0)
		return -EPROTO;

	if (command >= SPA_N_ELEMENTS(ext_stream_restore))
		return -ENOTSUP;
	if (ext_stream_restore[command].process == NULL)
		return -EPROTO;

	pw_log_info(NAME" %p: [EXT_STREAM_RESTORE_%s] %s tag:%u", impl, client->name,
			ext_stream_restore[command].name, tag);

	return ext_stream_restore[command].process(client, command, tag, m);
}
